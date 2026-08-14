#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>
#include <time.h>

#include "ACAP.h"
#include "occupancy.h"

#define OCCUPANCY_MAX_ITEMS 10
#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }

typedef struct {
	char scenario[64];
	ClassSelection classes;
	char value[16];
} ScenarioPublishConfig;

typedef struct {
	char scenario[64];
	char event_topic[128];
	char start[40];
	char end[40];
	int min_human;
	int min_car;
	int min_bike;
	int min_bus;
	int min_truck;
	int min_other;
	int max_human;
	int max_car;
	int max_bike;
	int max_bus;
	int max_truck;
	int max_other;
	double average_human;
	double average_car;
	double average_bike;
	double average_bus;
	double average_truck;
	double average_other;
	time_t last_update;
	int has_sample;
} OccupancyState;

static int g_occupancy_enabled = 0;
static int g_occupancy_interval_minutes = 15;
static int g_occupancy_port = 2;
static char g_occupancy_value[16] = "average";
static ClassSelection g_occupancy_classes = {1, 1, 1, 1, 1, 1};
static ScenarioPublishConfig g_occupancy_publish[OCCUPANCY_MAX_ITEMS];
static int g_occupancy_publish_count = 0;
static time_t g_next_occupancy_publish_time = 0;

static OccupancyState g_occupancy[OCCUPANCY_MAX_ITEMS];
static int g_occupancy_count = 0;
static pthread_mutex_t g_occupancy_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_occupancy_config_mutex = PTHREAD_MUTEX_INITIALIZER;

static int JSON_Int(cJSON* object, const char* key) {
	cJSON* item = cJSON_GetObjectItem(object, key);
	return item && cJSON_IsNumber(item) ? item->valueint : 0;
}

static double JSON_Double(cJSON* object, const char* key) {
	cJSON* item = cJSON_GetObjectItem(object, key);
	return item && cJSON_IsNumber(item) ? item->valuedouble : 0.0;
}

static void Copy_JSON_String(cJSON* object, const char* key, char* target, size_t target_size) {
	cJSON* item = cJSON_GetObjectItem(object, key);
	if (item && item->valuestring && target && target_size > 0) {
		strncpy(target, item->valuestring, target_size - 1);
		target[target_size - 1] = '\0';
	}
}

static int String_Ends_With(const char* text, const char* suffix) {
	if (!text || !suffix) return 0;
	size_t text_len = strlen(text);
	size_t suffix_len = strlen(suffix);
	return text_len >= suffix_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

static int Clamp_Publish_Interval(int minutes) {
	if (minutes < 1) return 1;
	if (minutes > 60) return 60;
	return minutes;
}

int Occupancy_Is_Valid_Value(const char* value) {
	return value && (strcmp(value, "min") == 0 || strcmp(value, "max") == 0 || strcmp(value, "average") == 0);
}

int Occupancy_Is_Scenario_Type(const char* scenario_type) {
	return scenario_type && (strcmp(scenario_type, "OccupancyInArea") == 0 || strcmp(scenario_type, "occupancyInArea") == 0);
}

void Append_U8(unsigned char* buffer, size_t* offset, uint8_t value) {
	buffer[(*offset)++] = value;
}

uint8_t Occupancy_Encode_U8(double value) {
	double rounded = round(value);
	if (rounded < 0.0) return 0;
	if (rounded > 255.0) return 255;
	return (uint8_t)rounded;
}

uint8_t Occupancy_Value_Type_Code(const char* value_type) {
	if (value_type && strcmp(value_type, "min") == 0) return 1;
	if (value_type && strcmp(value_type, "average") == 0) return 2;
	return 0;
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

static void Load_Scenario_Publish_Config(cJSON* scenarios_json, ScenarioPublishConfig* configs, int* count, ClassSelection default_classes, const char* default_value) {
	if (!configs || !count) return;
	*count = 0;
	if (!scenarios_json || !cJSON_IsObject(scenarios_json)) return;

	cJSON* item = scenarios_json->child;
	while (item && *count < OCCUPANCY_MAX_ITEMS) {
		if (item->string && cJSON_IsObject(item)) {
			ScenarioPublishConfig* config = &configs[*count];
			memset(config, 0, sizeof(ScenarioPublishConfig));
			strncpy(config->scenario, item->string, sizeof(config->scenario) - 1);
			config->classes = default_classes;
			snprintf(config->value, sizeof(config->value), "%s", default_value && Occupancy_Is_Valid_Value(default_value) ? default_value : "average");
			Load_Class_Selection(cJSON_GetObjectItem(item, "classes"), &config->classes);
			cJSON* value = cJSON_GetObjectItem(item, "value");
			if (value && value->valuestring && Occupancy_Is_Valid_Value(value->valuestring)) {
				strncpy(config->value, value->valuestring, sizeof(config->value) - 1);
				config->value[sizeof(config->value) - 1] = '\0';
			}
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

static ScenarioPublishConfig Occupancy_Config_For_Scenario(const char* scenario) {
	ScenarioPublishConfig result;
	memset(&result, 0, sizeof(result));
	if (scenario) strncpy(result.scenario, scenario, sizeof(result.scenario) - 1);
	result.classes = g_occupancy_classes;
	snprintf(result.value, sizeof(result.value), "%s", g_occupancy_value);
	ScenarioPublishConfig* config = Find_Scenario_Config(g_occupancy_publish, g_occupancy_publish_count, scenario);
	if (config) result = *config;
	return result;
}

static OccupancyState* Find_Or_Create_Occupancy(const char* scenario) {
	for (int i = 0; i < g_occupancy_count; i++) {
		if (strcmp(g_occupancy[i].scenario, scenario) == 0) return &g_occupancy[i];
	}
	if (g_occupancy_count < OCCUPANCY_MAX_ITEMS) {
		OccupancyState* occupancy = &g_occupancy[g_occupancy_count];
		memset(occupancy, 0, sizeof(OccupancyState));
		strncpy(occupancy->scenario, scenario, sizeof(occupancy->scenario) - 1);
		g_occupancy_count++;
		LOG("Created new occupancy state for scenario: %s\n", scenario);
		return occupancy;
	}
	LOG_WARN("Too many occupancy scenarios, cannot create for %s\n", scenario);
	return NULL;
}

static double Occupancy_Class_Value(const OccupancyState* occupancy, const char* occupancy_value, const char* class_name) {
	if (!occupancy || !occupancy_value || !class_name) return 0.0;
	if (strcmp(class_name, "human") == 0) {
		if (strcmp(occupancy_value, "min") == 0) return occupancy->min_human;
		if (strcmp(occupancy_value, "max") == 0) return occupancy->max_human;
		return occupancy->average_human;
	}
	if (strcmp(class_name, "car") == 0) {
		if (strcmp(occupancy_value, "min") == 0) return occupancy->min_car;
		if (strcmp(occupancy_value, "max") == 0) return occupancy->max_car;
		return occupancy->average_car;
	}
	if (strcmp(class_name, "bike") == 0) {
		if (strcmp(occupancy_value, "min") == 0) return occupancy->min_bike;
		if (strcmp(occupancy_value, "max") == 0) return occupancy->max_bike;
		return occupancy->average_bike;
	}
	if (strcmp(class_name, "bus") == 0) {
		if (strcmp(occupancy_value, "min") == 0) return occupancy->min_bus;
		if (strcmp(occupancy_value, "max") == 0) return occupancy->max_bus;
		return occupancy->average_bus;
	}
	if (strcmp(class_name, "truck") == 0) {
		if (strcmp(occupancy_value, "min") == 0) return occupancy->min_truck;
		if (strcmp(occupancy_value, "max") == 0) return occupancy->max_truck;
		return occupancy->average_truck;
	}
	if (strcmp(class_name, "other") == 0) {
		if (strcmp(occupancy_value, "min") == 0) return occupancy->min_other;
		if (strcmp(occupancy_value, "max") == 0) return occupancy->max_other;
		return occupancy->average_other;
	}
	return 0.0;
}

static void Add_Occupancy_Status_Label(cJSON* labels, const char* key, const char* label, double value) {
	cJSON* item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "key", key);
	cJSON_AddStringToObject(item, "label", label);
	cJSON_AddNumberToObject(item, "value", value);
	cJSON_AddItemToArray(labels, item);
}

static void Add_Selected_Occupancy_Status_Labels(cJSON* labels, const OccupancyState* occupancy, ScenarioPublishConfig publish_config) {
	if (publish_config.classes.human) Add_Occupancy_Status_Label(labels, "human", "Humans", Occupancy_Class_Value(occupancy, publish_config.value, "human"));
	if (publish_config.classes.car) Add_Occupancy_Status_Label(labels, "car", "Cars", Occupancy_Class_Value(occupancy, publish_config.value, "car"));
	if (publish_config.classes.bike) Add_Occupancy_Status_Label(labels, "bike", "Bikes", Occupancy_Class_Value(occupancy, publish_config.value, "bike"));
	if (publish_config.classes.bus) Add_Occupancy_Status_Label(labels, "bus", "Buses", Occupancy_Class_Value(occupancy, publish_config.value, "bus"));
	if (publish_config.classes.truck) Add_Occupancy_Status_Label(labels, "truck", "Trucks", Occupancy_Class_Value(occupancy, publish_config.value, "truck"));
	if (publish_config.classes.other) Add_Occupancy_Status_Label(labels, "other", "Other", Occupancy_Class_Value(occupancy, publish_config.value, "other"));
}

void Occupancy_Load_Config(cJSON* config, int fixed_port) {
	if (!config) return;
	pthread_mutex_lock(&g_occupancy_config_mutex);
	cJSON* enabled = cJSON_GetObjectItem(config, "enabled");
	if (enabled) g_occupancy_enabled = cJSON_IsTrue(enabled);
	cJSON* interval = cJSON_GetObjectItem(config, "intervalMinutes");
	if (interval) g_occupancy_interval_minutes = Clamp_Publish_Interval(interval->valueint);
	g_occupancy_port = fixed_port;
	cJSON* value = cJSON_GetObjectItem(config, "value");
	if (value && value->valuestring && Occupancy_Is_Valid_Value(value->valuestring)) {
		strncpy(g_occupancy_value, value->valuestring, sizeof(g_occupancy_value) - 1);
		g_occupancy_value[sizeof(g_occupancy_value) - 1] = '\0';
	}
	Load_Class_Selection(cJSON_GetObjectItem(config, "classes"), &g_occupancy_classes);
	Load_Scenario_Publish_Config(cJSON_GetObjectItem(config, "scenarios"), g_occupancy_publish, &g_occupancy_publish_count, g_occupancy_classes, g_occupancy_value);
	pthread_mutex_unlock(&g_occupancy_config_mutex);
}

int Occupancy_Enabled(void) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	int enabled = g_occupancy_enabled;
	pthread_mutex_unlock(&g_occupancy_config_mutex);
	return enabled;
}

int Occupancy_Port(void) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	int port = g_occupancy_port;
	pthread_mutex_unlock(&g_occupancy_config_mutex);
	return port;
}

int Occupancy_Interval_Minutes(void) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	int interval = g_occupancy_interval_minutes;
	pthread_mutex_unlock(&g_occupancy_config_mutex);
	return interval;
}

const char* Occupancy_Value(void) {
	return g_occupancy_value;
}

time_t Occupancy_Next_Publish_Time(void) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	time_t next = g_next_occupancy_publish_time;
	pthread_mutex_unlock(&g_occupancy_config_mutex);
	return next;
}

void Occupancy_Reset_Schedule(time_t now) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	g_next_occupancy_publish_time = g_occupancy_enabled ? now + (g_occupancy_interval_minutes * 60) : 0;
	pthread_mutex_unlock(&g_occupancy_config_mutex);
}

void Occupancy_Mark_Published(time_t now) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	g_next_occupancy_publish_time = now + (g_occupancy_interval_minutes * 60);
	pthread_mutex_unlock(&g_occupancy_config_mutex);
}

void Occupancy_Set_Interval_Minutes(int minutes) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	g_occupancy_interval_minutes = Clamp_Publish_Interval(minutes);
	pthread_mutex_unlock(&g_occupancy_config_mutex);
}

void Occupancy_Process_AOA_Event(cJSON* event) {
	cJSON* scenario_type = cJSON_GetObjectItem(event, "scenarioType");
	if (!scenario_type || !scenario_type->valuestring || !Occupancy_Is_Scenario_Type(scenario_type->valuestring)) return;
	cJSON* scenario_name = cJSON_GetObjectItem(event, "scenario");
	if (!scenario_name || !scenario_name->valuestring) {
		LOG_WARN("OccupancyInArea event missing scenario name\n");
		return;
	}
	cJSON* event_topic = cJSON_GetObjectItem(event, "event");
	if (!event_topic || !event_topic->valuestring || !String_Ends_With(event_topic->valuestring, "EventInterval")) return;
	char* event_json = cJSON_PrintUnformatted(event);
	if (event_json) {
		LOG("OccupancyInArea raw event: %s\n", event_json);
		free(event_json);
	}
	cJSON* end = cJSON_GetObjectItem(event, "end");
	if (!end || !end->valuestring) {
		LOG_WARN("OccupancyInArea event for %s missing end timestamp - ignored\n", scenario_name->valuestring);
		return;
	}
	pthread_mutex_lock(&g_occupancy_mutex);
	OccupancyState* occupancy = Find_Or_Create_Occupancy(scenario_name->valuestring);
	if (!occupancy) {
		pthread_mutex_unlock(&g_occupancy_mutex);
		return;
	}
	Copy_JSON_String(event, "start", occupancy->start, sizeof(occupancy->start));
	Copy_JSON_String(event, "end", occupancy->end, sizeof(occupancy->end));
	Copy_JSON_String(event, "event", occupancy->event_topic, sizeof(occupancy->event_topic));
	occupancy->min_human = JSON_Int(event, "minHuman");
	occupancy->min_car = JSON_Int(event, "minCar");
	occupancy->min_bike = JSON_Int(event, "minBike");
	occupancy->min_bus = JSON_Int(event, "minBus");
	occupancy->min_truck = JSON_Int(event, "minTruck");
	occupancy->min_other = JSON_Int(event, "minOtherVehicle");
	occupancy->max_human = JSON_Int(event, "maxHuman");
	occupancy->max_car = JSON_Int(event, "maxCar");
	occupancy->max_bike = JSON_Int(event, "maxBike");
	occupancy->max_bus = JSON_Int(event, "maxBus");
	occupancy->max_truck = JSON_Int(event, "maxTruck");
	occupancy->max_other = JSON_Int(event, "maxOtherVehicle");
	occupancy->average_human = JSON_Double(event, "averageHuman");
	occupancy->average_car = JSON_Double(event, "averageCar");
	occupancy->average_bike = JSON_Double(event, "averageBike");
	occupancy->average_bus = JSON_Double(event, "averageBus");
	occupancy->average_truck = JSON_Double(event, "averageTruck");
	occupancy->average_other = JSON_Double(event, "averageOtherVehicle");
	occupancy->last_update = time(NULL);
	occupancy->has_sample = 1;
	LOG("Occupancy %s event end=%s: car min=%d max=%d avg=%.2f, human min=%d max=%d avg=%.2f\n", occupancy->scenario, occupancy->end, occupancy->min_car, occupancy->max_car, occupancy->average_car, occupancy->min_human, occupancy->max_human, occupancy->average_human);
	pthread_mutex_unlock(&g_occupancy_mutex);
	Occupancy_Update_ACAP_Status();
}

void Occupancy_Update_ACAP_Status(void) {
	cJSON* areas = cJSON_CreateArray();
	if (!areas) return;
	pthread_mutex_lock(&g_occupancy_mutex);
	for (int i = 0; i < g_occupancy_count; i++) {
		OccupancyState* occupancy = &g_occupancy[i];
		ScenarioPublishConfig publish_config = Occupancy_Config_For_Scenario(occupancy->scenario);
		cJSON* area = cJSON_CreateObject();
		cJSON_AddStringToObject(area, "scenario", occupancy->scenario);
		cJSON_AddStringToObject(area, "event", occupancy->event_topic);
		cJSON_AddBoolToObject(area, "hasSample", occupancy->has_sample);
		cJSON_AddStringToObject(area, "timestamp", occupancy->end);
		cJSON_AddStringToObject(area, "valueType", publish_config.value);
		cJSON* labels = cJSON_CreateArray();
		if (occupancy->has_sample) Add_Selected_Occupancy_Status_Labels(labels, occupancy, publish_config);
		cJSON_AddItemToObject(area, "labels", labels);
		cJSON_AddItemToArray(areas, area);
	}
	pthread_mutex_unlock(&g_occupancy_mutex);
	ACAP_STATUS_SetObject("occupancy", "areas", areas);
	cJSON_Delete(areas);
}

int Occupancy_Count(void) {
	pthread_mutex_lock(&g_occupancy_mutex);
	int count = g_occupancy_count;
	pthread_mutex_unlock(&g_occupancy_mutex);
	return count;
}

void Occupancy_Add_Status_JSON(cJSON* occupancy_array) {
	pthread_mutex_lock(&g_occupancy_mutex);
	for (int i = 0; i < g_occupancy_count; i++) {
		OccupancyState* occupancy = &g_occupancy[i];
		cJSON* item = cJSON_CreateObject();
		ScenarioPublishConfig publish_config = Occupancy_Config_For_Scenario(occupancy->scenario);
		cJSON_AddStringToObject(item, "scenario", occupancy->scenario);
		cJSON_AddStringToObject(item, "start", occupancy->start);
		cJSON_AddStringToObject(item, "end", occupancy->end);
		cJSON_AddNumberToObject(item, "lastUpdate", (double)occupancy->last_update);
		cJSON_AddBoolToObject(item, "hasSample", occupancy->has_sample);
		cJSON_AddNumberToObject(item, "minHuman", occupancy->min_human);
		cJSON_AddNumberToObject(item, "minCar", occupancy->min_car);
		cJSON_AddNumberToObject(item, "minBike", occupancy->min_bike);
		cJSON_AddNumberToObject(item, "minBus", occupancy->min_bus);
		cJSON_AddNumberToObject(item, "minTruck", occupancy->min_truck);
		cJSON_AddNumberToObject(item, "minOther", occupancy->min_other);
		cJSON_AddNumberToObject(item, "maxHuman", occupancy->max_human);
		cJSON_AddNumberToObject(item, "maxCar", occupancy->max_car);
		cJSON_AddNumberToObject(item, "maxBike", occupancy->max_bike);
		cJSON_AddNumberToObject(item, "maxBus", occupancy->max_bus);
		cJSON_AddNumberToObject(item, "maxTruck", occupancy->max_truck);
		cJSON_AddNumberToObject(item, "maxOther", occupancy->max_other);
		cJSON_AddNumberToObject(item, "averageHuman", occupancy->average_human);
		cJSON_AddNumberToObject(item, "averageCar", occupancy->average_car);
		cJSON_AddNumberToObject(item, "averageBike", occupancy->average_bike);
		cJSON_AddNumberToObject(item, "averageBus", occupancy->average_bus);
		cJSON_AddNumberToObject(item, "averageTruck", occupancy->average_truck);
		cJSON_AddNumberToObject(item, "averageOther", occupancy->average_other);
		cJSON_AddStringToObject(item, "selectedValueType", publish_config.value);
		cJSON_AddNumberToObject(item, "selectedHuman", Occupancy_Class_Value(occupancy, publish_config.value, "human"));
		cJSON_AddNumberToObject(item, "selectedCar", Occupancy_Class_Value(occupancy, publish_config.value, "car"));
		cJSON_AddNumberToObject(item, "selectedBike", Occupancy_Class_Value(occupancy, publish_config.value, "bike"));
		cJSON_AddNumberToObject(item, "selectedBus", Occupancy_Class_Value(occupancy, publish_config.value, "bus"));
		cJSON_AddNumberToObject(item, "selectedTruck", Occupancy_Class_Value(occupancy, publish_config.value, "truck"));
		cJSON_AddNumberToObject(item, "selectedOther", Occupancy_Class_Value(occupancy, publish_config.value, "other"));
		cJSON* publish_classes = cJSON_CreateObject();
		cJSON_AddBoolToObject(publish_classes, "human", publish_config.classes.human);
		cJSON_AddBoolToObject(publish_classes, "car", publish_config.classes.car);
		cJSON_AddBoolToObject(publish_classes, "bike", publish_config.classes.bike);
		cJSON_AddBoolToObject(publish_classes, "bus", publish_config.classes.bus);
		cJSON_AddBoolToObject(publish_classes, "truck", publish_config.classes.truck);
		cJSON_AddBoolToObject(publish_classes, "other", publish_config.classes.other);
		cJSON_AddItemToObject(item, "publishClasses", publish_classes);
		cJSON_AddItemToArray(occupancy_array, item);
	}
	pthread_mutex_unlock(&g_occupancy_mutex);
}

void Occupancy_Add_Publish_JSON(cJSON* publish, time_t now) {
	pthread_mutex_lock(&g_occupancy_config_mutex);
	cJSON* occupancy = cJSON_CreateObject();
	cJSON_AddBoolToObject(occupancy, "enabled", g_occupancy_enabled);
	cJSON_AddNumberToObject(occupancy, "port", g_occupancy_port);
	cJSON_AddNumberToObject(occupancy, "intervalMinutes", g_occupancy_interval_minutes);
	cJSON_AddStringToObject(occupancy, "value", g_occupancy_value);
	cJSON_AddNumberToObject(occupancy, "nextPublishTime", (double)g_next_occupancy_publish_time);
	cJSON_AddNumberToObject(occupancy, "secondsUntilPublish", g_occupancy_enabled && g_next_occupancy_publish_time > now ? (int)(g_next_occupancy_publish_time - now) : 0);
	pthread_mutex_unlock(&g_occupancy_config_mutex);
	cJSON_AddItemToObject(publish, "occupancy", occupancy);
}

void Occupancy_Build_Decoder_Definitions(cJSON* definitions, int port) {
	pthread_mutex_lock(&g_occupancy_mutex);
	for (int i = 0; i < g_occupancy_count; i++) {
		ScenarioPublishConfig publish_config = Occupancy_Config_For_Scenario(g_occupancy[i].scenario);
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", g_occupancy[i].scenario);
		cJSON_AddStringToObject(item, "event", g_occupancy[i].event_topic);
		cJSON_AddNumberToObject(item, "port", port);
		cJSON_AddStringToObject(item, "value", publish_config.value);
		cJSON_AddNumberToObject(item, "valueTypeCode", Occupancy_Value_Type_Code(publish_config.value));
		cJSON_AddItemToObject(item, "classes", Class_Array_JSON(publish_config.classes));
		cJSON_AddItemToArray(definitions, item);
	}
	pthread_mutex_unlock(&g_occupancy_mutex);

	pthread_mutex_lock(&g_occupancy_config_mutex);
	for (int i = 0; i < g_occupancy_publish_count; i++) {
		ScenarioPublishConfig* publish_config = &g_occupancy_publish[i];
		int exists = 0;
		cJSON* def = NULL;
		cJSON_ArrayForEach(def, definitions) {
			cJSON* name = cJSON_GetObjectItem(def, "name");
			if (name && name->valuestring && strcmp(name->valuestring, publish_config->scenario) == 0) {
				exists = 1;
				break;
			}
		}
		if (exists) continue;
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", publish_config->scenario);
		cJSON_AddStringToObject(item, "event", "");
		cJSON_AddNumberToObject(item, "port", port);
		cJSON_AddStringToObject(item, "value", publish_config->value);
		cJSON_AddNumberToObject(item, "valueTypeCode", Occupancy_Value_Type_Code(publish_config->value));
		cJSON_AddItemToObject(item, "classes", Class_Array_JSON(publish_config->classes));
		cJSON_AddItemToArray(definitions, item);
	}
	pthread_mutex_unlock(&g_occupancy_config_mutex);
}

int Occupancy_Build_Payload(unsigned char** out_buffer, size_t* out_size, int* out_sample_count, int* out_class_count) {
	if (!out_buffer || !out_size || !out_sample_count || !out_class_count) return 0;
	*out_buffer = NULL;
	*out_size = 0;
	*out_sample_count = 0;
	*out_class_count = 0;
	cJSON* status_areas = ACAP_STATUS_Object("occupancy", "areas");
	cJSON* areas = status_areas ? cJSON_Duplicate(status_areas, 1) : NULL;
	if (!areas) {
		LOG_WARN("No occupancy status available to publish\n");
		return 0;
	}
	int occupancy_sample_count = 0;
	int total_class_count = 0;
	int total_header_bytes = 0;
	cJSON* area = NULL;
	cJSON_ArrayForEach(area, areas) {
		cJSON* has_sample = cJSON_GetObjectItem(area, "hasSample");
		if (!cJSON_IsTrue(has_sample)) continue;
		cJSON* labels = cJSON_GetObjectItem(area, "labels");
		int class_count = labels && cJSON_IsArray(labels) ? cJSON_GetArraySize(labels) : 0;
		if (class_count <= 0) continue;
		occupancy_sample_count++;
		total_class_count += class_count;
		total_header_bytes += 2;
	}
	if (occupancy_sample_count == 0) {
		cJSON_Delete(areas);
		LOG_WARN("No occupancy samples/classes to publish\n");
		return 0;
	}
	size_t buffer_size = total_header_bytes + total_class_count;
	unsigned char* buffer = malloc(buffer_size);
	if (!buffer) {
		cJSON_Delete(areas);
		LOG_WARN("Failed to allocate buffer\n");
		return 0;
	}
	size_t offset = 0;
	cJSON_ArrayForEach(area, areas) {
		cJSON* has_sample = cJSON_GetObjectItem(area, "hasSample");
		if (!cJSON_IsTrue(has_sample)) continue;
		cJSON* labels = cJSON_GetObjectItem(area, "labels");
		if (!labels || !cJSON_IsArray(labels)) continue;
		int label_count = cJSON_GetArraySize(labels);
		if (label_count <= 0) continue;
		if (label_count > 255) label_count = 255;
		cJSON* value_type = cJSON_GetObjectItem(area, "valueType");
		Append_U8(buffer, &offset, (uint8_t)label_count);
		Append_U8(buffer, &offset, Occupancy_Value_Type_Code(value_type && cJSON_IsString(value_type) ? value_type->valuestring : NULL));
		cJSON* label = NULL;
		int written = 0;
		cJSON_ArrayForEach(label, labels) {
			if (written >= label_count) break;
			cJSON* value = cJSON_GetObjectItem(label, "value");
			Append_U8(buffer, &offset, Occupancy_Encode_U8(value && cJSON_IsNumber(value) ? value->valuedouble : 0.0));
			written++;
		}
	}
	cJSON_Delete(areas);
	*out_buffer = buffer;
	*out_size = buffer_size;
	*out_sample_count = occupancy_sample_count;
	*out_class_count = total_class_count;
	return 1;
}
