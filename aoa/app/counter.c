#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>
#include <time.h>

#include "ACAP.h"
#include "counter.h"

#define COUNTER_MAX_ITEMS 10
#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }

typedef struct {
	char scenario[64];
	int aoa_reference_total;
	int aoa_reference_human;
	int aoa_reference_car;
	int aoa_reference_bike;
	int aoa_reference_bus;
	int aoa_reference_truck;
	int aoa_reference_other;
	int internal_total;
	int internal_human;
	int internal_car;
	int internal_bike;
	int internal_bus;
	int internal_truck;
	int internal_other;
	int has_reference;
} CounterState;

typedef struct {
	char scenario[64];
	ClassSelection classes;
	char value[16];
} ScenarioPublishConfig;

static int g_counting_enabled = 1;
static int g_counting_interval_minutes = 15;
static int g_counting_port = 1;
static ClassSelection g_counting_classes = {1, 1, 1, 1, 1, 1};
static ScenarioPublishConfig g_counting_publish[COUNTER_MAX_ITEMS];
static int g_counting_publish_count = 0;
static time_t g_next_counting_publish_time = 0;

static CounterState g_counters[COUNTER_MAX_ITEMS];
static int g_counter_count = 0;
static time_t g_last_save_time = 0;
static pthread_mutex_t g_counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_counting_config_mutex = PTHREAD_MUTEX_INITIALIZER;

static int Clamp_Publish_Interval(int minutes) {
	if (minutes < 1) return 1;
	if (minutes > 60) return 60;
	return minutes;
}

int Class_Count(ClassSelection classes) {
	return classes.human + classes.car + classes.bike + classes.bus + classes.truck + classes.other;
}

uint16_t Wrap_U16_Int(int value) {
	if (value < 0) return 0;
	return (uint16_t)(value & 0xFFFF);
}

void Append_U16(unsigned char* buffer, size_t* offset, uint16_t value) {
	buffer[(*offset)++] = value & 0xFF;
	buffer[(*offset)++] = (value >> 8) & 0xFF;
}

static void Load_Class_Selection(cJSON* classes_json, ClassSelection* classes) {
	if (!classes_json || !classes) return;
	cJSON* human = cJSON_GetObjectItem(classes_json, "human");
	if (human) classes->human = cJSON_IsTrue(human);
	cJSON* car = cJSON_GetObjectItem(classes_json, "car");
	if (car) classes->car = cJSON_IsTrue(car);
	cJSON* bike = cJSON_GetObjectItem(classes_json, "bike");
	if (bike) classes->bike = cJSON_IsTrue(bike);
	cJSON* bus = cJSON_GetObjectItem(classes_json, "bus");
	if (bus) classes->bus = cJSON_IsTrue(bus);
	cJSON* truck = cJSON_GetObjectItem(classes_json, "truck");
	if (truck) classes->truck = cJSON_IsTrue(truck);
	cJSON* other = cJSON_GetObjectItem(classes_json, "other");
	if (other) classes->other = cJSON_IsTrue(other);
}

static void Load_Scenario_Publish_Config(cJSON* scenarios_json, ScenarioPublishConfig* configs, int* count, ClassSelection default_classes) {
	if (!configs || !count) return;
	*count = 0;
	if (!scenarios_json || !cJSON_IsObject(scenarios_json)) return;

	cJSON* item = scenarios_json->child;
	while (item && *count < COUNTER_MAX_ITEMS) {
		if (item->string && cJSON_IsObject(item)) {
			ScenarioPublishConfig* config = &configs[*count];
			memset(config, 0, sizeof(ScenarioPublishConfig));
			strncpy(config->scenario, item->string, sizeof(config->scenario) - 1);
			config->classes = default_classes;
			Load_Class_Selection(cJSON_GetObjectItem(item, "classes"), &config->classes);
			(*count)++;
		}
		item = item->next;
	}
}

static ScenarioPublishConfig* Find_Scenario_Config(ScenarioPublishConfig* configs, int count, const char* scenario) {
	if (!configs || !scenario) return NULL;
	for (int i = 0; i < count; i++) {
		if (strcmp(configs[i].scenario, scenario) == 0) return &configs[i];
	}
	return NULL;
}

static ClassSelection Counting_Classes_For_Scenario(const char* scenario) {
	ScenarioPublishConfig* config = Find_Scenario_Config(g_counting_publish, g_counting_publish_count, scenario);
	return config ? config->classes : g_counting_classes;
}

static CounterState* Find_Or_Create_Counter(const char* scenario) {
	for (int i = 0; i < g_counter_count; i++) {
		if (strcmp(g_counters[i].scenario, scenario) == 0) return &g_counters[i];
	}

	if (g_counter_count < COUNTER_MAX_ITEMS) {
		CounterState* counter = &g_counters[g_counter_count];
		memset(counter, 0, sizeof(CounterState));
		strncpy(counter->scenario, scenario, sizeof(counter->scenario) - 1);
		g_counter_count++;
		LOG("Created new counter for scenario: %s\n", scenario);
		return counter;
	}

	LOG_WARN("Too many counters, cannot create for %s\n", scenario);
	return NULL;
}

void Counting_Load_Config(cJSON* config, int fixed_port) {
	if (!config) return;
	pthread_mutex_lock(&g_counting_config_mutex);
	cJSON* enabled = cJSON_GetObjectItem(config, "enabled");
	if (enabled) g_counting_enabled = cJSON_IsTrue(enabled);
	cJSON* interval = cJSON_GetObjectItem(config, "intervalMinutes");
	if (interval) g_counting_interval_minutes = Clamp_Publish_Interval(interval->valueint);
	g_counting_port = fixed_port;
	Load_Class_Selection(cJSON_GetObjectItem(config, "classes"), &g_counting_classes);
	Load_Scenario_Publish_Config(cJSON_GetObjectItem(config, "scenarios"), g_counting_publish, &g_counting_publish_count, g_counting_classes);
	pthread_mutex_unlock(&g_counting_config_mutex);
}

int Counting_Enabled(void) {
	pthread_mutex_lock(&g_counting_config_mutex);
	int enabled = g_counting_enabled;
	pthread_mutex_unlock(&g_counting_config_mutex);
	return enabled;
}

int Counting_Port(void) {
	pthread_mutex_lock(&g_counting_config_mutex);
	int port = g_counting_port;
	pthread_mutex_unlock(&g_counting_config_mutex);
	return port;
}

int Counting_Interval_Minutes(void) {
	pthread_mutex_lock(&g_counting_config_mutex);
	int interval = g_counting_interval_minutes;
	pthread_mutex_unlock(&g_counting_config_mutex);
	return interval;
}

time_t Counting_Next_Publish_Time(void) {
	pthread_mutex_lock(&g_counting_config_mutex);
	time_t next = g_next_counting_publish_time;
	pthread_mutex_unlock(&g_counting_config_mutex);
	return next;
}

void Counting_Reset_Schedule(time_t now) {
	pthread_mutex_lock(&g_counting_config_mutex);
	g_next_counting_publish_time = g_counting_enabled ? now + (g_counting_interval_minutes * 60) : 0;
	pthread_mutex_unlock(&g_counting_config_mutex);
}

void Counting_Mark_Published(time_t now) {
	pthread_mutex_lock(&g_counting_config_mutex);
	g_next_counting_publish_time = now + (g_counting_interval_minutes * 60);
	pthread_mutex_unlock(&g_counting_config_mutex);
}

void Counting_Set_Interval_Minutes(int minutes) {
	pthread_mutex_lock(&g_counting_config_mutex);
	g_counting_interval_minutes = Clamp_Publish_Interval(minutes);
	pthread_mutex_unlock(&g_counting_config_mutex);
}

void Counting_Process_AOA_Event(cJSON* event) {
	cJSON* scenario_name = cJSON_GetObjectItem(event, "scenario");
	if (!scenario_name || !scenario_name->valuestring) {
		LOG_WARN("CrosslineCounting event missing scenario name\n");
		return;
	}

	pthread_mutex_lock(&g_counter_mutex);
	CounterState* counter = Find_Or_Create_Counter(scenario_name->valuestring);
	if (!counter) {
		pthread_mutex_unlock(&g_counter_mutex);
		return;
	}

	int aoa_total = 0, aoa_human = 0, aoa_car = 0, aoa_bike = 0;
	int aoa_bus = 0, aoa_truck = 0, aoa_other = 0;

	cJSON* total = cJSON_GetObjectItem(event, "total");
	if (total) aoa_total = total->valueint;
	cJSON* totalHuman = cJSON_GetObjectItem(event, "totalHuman");
	if (totalHuman) aoa_human = totalHuman->valueint;
	cJSON* totalCar = cJSON_GetObjectItem(event, "totalCar");
	if (totalCar) aoa_car = totalCar->valueint;
	cJSON* totalBike = cJSON_GetObjectItem(event, "totalBike");
	if (totalBike) aoa_bike = totalBike->valueint;
	cJSON* totalBus = cJSON_GetObjectItem(event, "totalBus");
	if (totalBus) aoa_bus = totalBus->valueint;
	cJSON* totalTruck = cJSON_GetObjectItem(event, "totalTruck");
	if (totalTruck) aoa_truck = totalTruck->valueint;
	cJSON* totalOther = cJSON_GetObjectItem(event, "totalOtherVehicle");
	if (totalOther) aoa_other = totalOther->valueint;

	cJSON* reason = cJSON_GetObjectItem(event, "reason");
	const char* reason_str = reason && reason->valuestring ? reason->valuestring : "unknown";

	if (!counter->has_reference) {
		counter->aoa_reference_total = aoa_total;
		counter->aoa_reference_human = aoa_human;
		counter->aoa_reference_car = aoa_car;
		counter->aoa_reference_bike = aoa_bike;
		counter->aoa_reference_bus = aoa_bus;
		counter->aoa_reference_truck = aoa_truck;
		counter->aoa_reference_other = aoa_other;
		counter->has_reference = 1;
		LOG("Set reference for %s: total=%d (reason: %s)\n", counter->scenario, aoa_total, reason_str);
	} else {
		int delta_total = aoa_total - counter->aoa_reference_total;
		int delta_human = aoa_human - counter->aoa_reference_human;
		int delta_car = aoa_car - counter->aoa_reference_car;
		int delta_bike = aoa_bike - counter->aoa_reference_bike;
		int delta_bus = aoa_bus - counter->aoa_reference_bus;
		int delta_truck = aoa_truck - counter->aoa_reference_truck;
		int delta_other = aoa_other - counter->aoa_reference_other;

		if (delta_total < 0 || delta_human < 0 || delta_car < 0 || delta_bike < 0 || delta_bus < 0 || delta_truck < 0 || delta_other < 0) {
			LOG_WARN("%s: AOA counter reset detected (delta=%d), updating reference\n", counter->scenario, delta_total);
			counter->aoa_reference_total = aoa_total;
			counter->aoa_reference_human = aoa_human;
			counter->aoa_reference_car = aoa_car;
			counter->aoa_reference_bike = aoa_bike;
			counter->aoa_reference_bus = aoa_bus;
			counter->aoa_reference_truck = aoa_truck;
			counter->aoa_reference_other = aoa_other;
		} else if (delta_total > 0) {
			counter->internal_total += delta_total;
			counter->internal_human += delta_human;
			counter->internal_car += delta_car;
			counter->internal_bike += delta_bike;
			counter->internal_bus += delta_bus;
			counter->internal_truck += delta_truck;
			counter->internal_other += delta_other;
			counter->aoa_reference_total = aoa_total;
			counter->aoa_reference_human = aoa_human;
			counter->aoa_reference_car = aoa_car;
			counter->aoa_reference_bike = aoa_bike;
			counter->aoa_reference_bus = aoa_bus;
			counter->aoa_reference_truck = aoa_truck;
			counter->aoa_reference_other = aoa_other;
			LOG("%s: +%d %s (internal: %d total, %d human, %d car)\n", counter->scenario, delta_total, reason_str, counter->internal_total, counter->internal_human, counter->internal_car);

			time_t now = time(NULL);
			if (now - g_last_save_time >= 60) {
				pthread_mutex_unlock(&g_counter_mutex);
				Counting_Save_To_File();
				return;
			}
		}
	}

	pthread_mutex_unlock(&g_counter_mutex);
}

void Counting_Load_From_File(void) {
	pthread_mutex_lock(&g_counter_mutex);
	cJSON* data = ACAP_FILE_Read("localdata/counters.json");
	if (!data) {
		LOG("No saved counters found, starting fresh\n");
		pthread_mutex_unlock(&g_counter_mutex);
		return;
	}

	g_counter_count = 0;
	cJSON* counter_item = data->child;
	while (counter_item && g_counter_count < COUNTER_MAX_ITEMS) {
		CounterState* counter = &g_counters[g_counter_count];
		memset(counter, 0, sizeof(CounterState));
		cJSON* scenario = cJSON_GetObjectItem(counter_item, "scenario");
		if (scenario && scenario->valuestring) strncpy(counter->scenario, scenario->valuestring, sizeof(counter->scenario) - 1);
		cJSON* total = cJSON_GetObjectItem(counter_item, "total");
		if (total) counter->internal_total = total->valueint;
		cJSON* human = cJSON_GetObjectItem(counter_item, "human");
		if (human) counter->internal_human = human->valueint;
		cJSON* car = cJSON_GetObjectItem(counter_item, "car");
		if (car) counter->internal_car = car->valueint;
		cJSON* bike = cJSON_GetObjectItem(counter_item, "bike");
		if (bike) counter->internal_bike = bike->valueint;
		cJSON* bus = cJSON_GetObjectItem(counter_item, "bus");
		if (bus) counter->internal_bus = bus->valueint;
		cJSON* truck = cJSON_GetObjectItem(counter_item, "truck");
		if (truck) counter->internal_truck = truck->valueint;
		cJSON* other = cJSON_GetObjectItem(counter_item, "other");
		if (other) counter->internal_other = other->valueint;
		counter->has_reference = 0;
		LOG("Loaded counter %s: total=%d, human=%d, car=%d\n", counter->scenario, counter->internal_total, counter->internal_human, counter->internal_car);
		g_counter_count++;
		counter_item = counter_item->next;
	}

	cJSON_Delete(data);
	pthread_mutex_unlock(&g_counter_mutex);
	LOG("Loaded %d counters from file\n", g_counter_count);
}

void Counting_Save_To_File(void) {
	pthread_mutex_lock(&g_counter_mutex);
	LOG("Saving %d counters to file...\n", g_counter_count);
	cJSON* root = cJSON_CreateArray();
	for (int i = 0; i < g_counter_count; i++) {
		CounterState* counter = &g_counters[i];
		LOG("  - %s (total=%d)\n", counter->scenario, counter->internal_total);
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "scenario", counter->scenario);
		cJSON_AddNumberToObject(item, "total", counter->internal_total);
		cJSON_AddNumberToObject(item, "human", counter->internal_human);
		cJSON_AddNumberToObject(item, "car", counter->internal_car);
		cJSON_AddNumberToObject(item, "bike", counter->internal_bike);
		cJSON_AddNumberToObject(item, "bus", counter->internal_bus);
		cJSON_AddNumberToObject(item, "truck", counter->internal_truck);
		cJSON_AddNumberToObject(item, "other", counter->internal_other);
		cJSON_AddItemToArray(root, item);
	}
	ACAP_FILE_Write("localdata/counters.json", root);
	cJSON_Delete(root);
	pthread_mutex_unlock(&g_counter_mutex);
	g_last_save_time = time(NULL);
	LOG("Counters saved to localdata/counters.json\n");
}

void Counting_Delete_By_Scenario(const char* scenario) {
	pthread_mutex_lock(&g_counter_mutex);
	int found_index = -1;
	for (int i = 0; i < g_counter_count; i++) {
		if (strcmp(g_counters[i].scenario, scenario) == 0) {
			found_index = i;
			break;
		}
	}
	if (found_index >= 0) {
		LOG("Deleting counter: %s\n", scenario);
		for (int i = found_index; i < g_counter_count - 1; i++) memcpy(&g_counters[i], &g_counters[i + 1], sizeof(CounterState));
		g_counter_count--;
		memset(&g_counters[g_counter_count], 0, sizeof(CounterState));
		pthread_mutex_unlock(&g_counter_mutex);
		Counting_Save_To_File();
	} else {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("Counter not found for deletion: %s\n", scenario);
	}
}

void Counting_Sync_With_AOA_List(cJSON* scenario_array) {
	if (!scenario_array || !cJSON_IsArray(scenario_array)) {
		LOG_WARN("Invalid scenario array for sync\n");
		return;
	}
	pthread_mutex_lock(&g_counter_mutex);
	int keep_count = 0;
	char* keep_list[COUNTER_MAX_ITEMS];
	cJSON* item = scenario_array->child;
	while (item && keep_count < COUNTER_MAX_ITEMS) {
		if (cJSON_IsString(item) && item->valuestring) keep_list[keep_count++] = item->valuestring;
		item = item->next;
	}
	int removed = 0;
	for (int i = g_counter_count - 1; i >= 0; i--) {
		int should_keep = 0;
		for (int j = 0; j < keep_count; j++) {
			if (strcmp(g_counters[i].scenario, keep_list[j]) == 0) {
				should_keep = 1;
				break;
			}
		}
		if (!should_keep) {
			LOG("Sync: Removing counter '%s' (not in AOA)\n", g_counters[i].scenario);
			for (int j = i; j < g_counter_count - 1; j++) memcpy(&g_counters[j], &g_counters[j + 1], sizeof(CounterState));
			g_counter_count--;
			removed++;
		}
	}
	int remaining = g_counter_count;
	pthread_mutex_unlock(&g_counter_mutex);
	if (removed > 0) {
		LOG("Sync: Removed %d counter(s), saving to file...\n", removed);
		Counting_Save_To_File();
		LOG("Sync: File saved with %d counters remaining\n", remaining);
	} else {
		LOG("Sync: All counters match AOA configuration (%d counters)\n", remaining);
	}
}

void Counting_Reset_All(void) {
	pthread_mutex_lock(&g_counter_mutex);
	for (int i = 0; i < g_counter_count; i++) {
		g_counters[i].internal_total = 0;
		g_counters[i].internal_human = 0;
		g_counters[i].internal_car = 0;
		g_counters[i].internal_bike = 0;
		g_counters[i].internal_bus = 0;
		g_counters[i].internal_truck = 0;
		g_counters[i].internal_other = 0;
		g_counters[i].has_reference = 0;
	}
	pthread_mutex_unlock(&g_counter_mutex);
}

int Counting_Set_Values_From_JSON(cJSON* body) {
	cJSON* scenario_json = cJSON_GetObjectItem(body, "scenario");
	if (!scenario_json || !scenario_json->valuestring || strlen(scenario_json->valuestring) == 0) return 0;
	const char* scenario = scenario_json->valuestring;
	pthread_mutex_lock(&g_counter_mutex);
	CounterState* counter = NULL;
	for (int i = 0; i < g_counter_count; i++) {
		if (strcmp(g_counters[i].scenario, scenario) == 0) {
			counter = &g_counters[i];
			break;
		}
	}
	if (!counter) {
		pthread_mutex_unlock(&g_counter_mutex);
		return 0;
	}
	cJSON* val;
	val = cJSON_GetObjectItem(body, "human");
	if (val && cJSON_IsNumber(val)) counter->internal_human = val->valueint;
	val = cJSON_GetObjectItem(body, "car");
	if (val && cJSON_IsNumber(val)) counter->internal_car = val->valueint;
	val = cJSON_GetObjectItem(body, "bike");
	if (val && cJSON_IsNumber(val)) counter->internal_bike = val->valueint;
	val = cJSON_GetObjectItem(body, "bus");
	if (val && cJSON_IsNumber(val)) counter->internal_bus = val->valueint;
	val = cJSON_GetObjectItem(body, "truck");
	if (val && cJSON_IsNumber(val)) counter->internal_truck = val->valueint;
	val = cJSON_GetObjectItem(body, "other");
	if (val && cJSON_IsNumber(val)) counter->internal_other = val->valueint;
	counter->internal_total = counter->internal_human + counter->internal_car + counter->internal_bike + counter->internal_bus + counter->internal_truck + counter->internal_other;
	counter->has_reference = 0;
	LOG("Set counters for %s: human=%d car=%d bike=%d bus=%d truck=%d other=%d total=%d\n", scenario, counter->internal_human, counter->internal_car, counter->internal_bike, counter->internal_bus, counter->internal_truck, counter->internal_other, counter->internal_total);
	pthread_mutex_unlock(&g_counter_mutex);
	Counting_Save_To_File();
	return 1;
}

int Counting_Count(void) {
	pthread_mutex_lock(&g_counter_mutex);
	int count = g_counter_count;
	pthread_mutex_unlock(&g_counter_mutex);
	return count;
}

void Counting_Add_Counters_JSON(cJSON* counters_array) {
	pthread_mutex_lock(&g_counter_mutex);
	for (int i = 0; i < g_counter_count; i++) {
		CounterState* counter = &g_counters[i];
		cJSON* item = cJSON_CreateObject();
		ClassSelection classes = Counting_Classes_For_Scenario(counter->scenario);
		cJSON_AddStringToObject(item, "scenario", counter->scenario);
		cJSON_AddNumberToObject(item, "total", counter->internal_total);
		cJSON_AddNumberToObject(item, "human", counter->internal_human);
		cJSON_AddNumberToObject(item, "car", counter->internal_car);
		cJSON_AddNumberToObject(item, "bike", counter->internal_bike);
		cJSON_AddNumberToObject(item, "bus", counter->internal_bus);
		cJSON_AddNumberToObject(item, "truck", counter->internal_truck);
		cJSON_AddNumberToObject(item, "other", counter->internal_other);
		cJSON* publish_classes = cJSON_CreateObject();
		cJSON_AddBoolToObject(publish_classes, "human", classes.human);
		cJSON_AddBoolToObject(publish_classes, "car", classes.car);
		cJSON_AddBoolToObject(publish_classes, "bike", classes.bike);
		cJSON_AddBoolToObject(publish_classes, "bus", classes.bus);
		cJSON_AddBoolToObject(publish_classes, "truck", classes.truck);
		cJSON_AddBoolToObject(publish_classes, "other", classes.other);
		cJSON_AddItemToObject(item, "publishClasses", publish_classes);
		cJSON_AddItemToArray(counters_array, item);
	}
	pthread_mutex_unlock(&g_counter_mutex);
}

void Counting_Add_Publish_JSON(cJSON* publish, time_t now) {
	pthread_mutex_lock(&g_counting_config_mutex);
	cJSON* counting = cJSON_CreateObject();
	cJSON_AddBoolToObject(counting, "enabled", g_counting_enabled);
	cJSON_AddNumberToObject(counting, "port", g_counting_port);
	cJSON_AddNumberToObject(counting, "intervalMinutes", g_counting_interval_minutes);
	cJSON_AddNumberToObject(counting, "nextPublishTime", (double)g_next_counting_publish_time);
	cJSON_AddNumberToObject(counting, "secondsUntilPublish", g_counting_enabled && g_next_counting_publish_time > now ? (int)(g_next_counting_publish_time - now) : 0);
	pthread_mutex_unlock(&g_counting_config_mutex);
	cJSON_AddItemToObject(publish, "counting", counting);
}

cJSON* Class_Array_JSON(ClassSelection classes) {
	cJSON* array = cJSON_CreateArray();
	if (classes.human) cJSON_AddItemToArray(array, cJSON_CreateString("human"));
	if (classes.car) cJSON_AddItemToArray(array, cJSON_CreateString("car"));
	if (classes.bike) cJSON_AddItemToArray(array, cJSON_CreateString("bike"));
	if (classes.bus) cJSON_AddItemToArray(array, cJSON_CreateString("bus"));
	if (classes.truck) cJSON_AddItemToArray(array, cJSON_CreateString("truck"));
	if (classes.other) cJSON_AddItemToArray(array, cJSON_CreateString("other"));
	return array;
}

void Counting_Build_Decoder_Definitions(cJSON* definitions, int port) {
	pthread_mutex_lock(&g_counter_mutex);
	for (int i = 0; i < g_counter_count; i++) {
		ClassSelection classes = Counting_Classes_For_Scenario(g_counters[i].scenario);
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", g_counters[i].scenario);
		cJSON_AddNumberToObject(item, "port", port);
		cJSON_AddItemToObject(item, "classes", Class_Array_JSON(classes));
		cJSON_AddItemToArray(definitions, item);
	}
	pthread_mutex_unlock(&g_counter_mutex);
}

int Counting_Build_Payload(unsigned char** out_buffer, size_t* out_size, int* out_counter_count, int* out_class_count) {
	if (!out_buffer || !out_size || !out_counter_count || !out_class_count) return 0;
	*out_buffer = NULL;
	*out_size = 0;
	*out_counter_count = 0;
	*out_class_count = 0;

	pthread_mutex_lock(&g_counter_mutex);
	if (g_counter_count == 0) {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("No counters to publish\n");
		return 0;
	}

	int published_counter_count = 0;
	int total_class_count = 0;
	for (int i = 0; i < g_counter_count; i++) {
		ClassSelection classes = Counting_Classes_For_Scenario(g_counters[i].scenario);
		int class_count = Class_Count(classes);
		if (class_count == 0) continue;
		if (!g_counters[i].has_reference) {
			LOG_WARN("%s: No current AOA counter values available yet; skipping counting publish\n", g_counters[i].scenario);
			pthread_mutex_unlock(&g_counter_mutex);
			return 0;
		}
		published_counter_count++;
		total_class_count += class_count;
	}
	if (total_class_count == 0) {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("No classes selected for counting publish\n");
		return 0;
	}

	size_t buffer_size = total_class_count * 2;
	unsigned char* buffer = malloc(buffer_size);
	if (!buffer) {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("Failed to allocate buffer\n");
		return 0;
	}

	size_t offset = 0;
	for (int i = 0; i < g_counter_count; i++) {
		CounterState* counter = &g_counters[i];
		ClassSelection classes = Counting_Classes_For_Scenario(counter->scenario);
		if (classes.human) Append_U16(buffer, &offset, Wrap_U16_Int(counter->aoa_reference_human));
		if (classes.car) Append_U16(buffer, &offset, Wrap_U16_Int(counter->aoa_reference_car));
		if (classes.bike) Append_U16(buffer, &offset, Wrap_U16_Int(counter->aoa_reference_bike));
		if (classes.bus) Append_U16(buffer, &offset, Wrap_U16_Int(counter->aoa_reference_bus));
		if (classes.truck) Append_U16(buffer, &offset, Wrap_U16_Int(counter->aoa_reference_truck));
		if (classes.other) Append_U16(buffer, &offset, Wrap_U16_Int(counter->aoa_reference_other));
	}
	pthread_mutex_unlock(&g_counter_mutex);

	*out_buffer = buffer;
	*out_size = buffer_size;
	*out_counter_count = published_counter_count;
	*out_class_count = total_class_count;
	return 1;
}
