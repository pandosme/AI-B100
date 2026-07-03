#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <math.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ACAP.h"
#include "cJSON.h"
#include "B100.h"

#define APP_PACKAGE	"aib100"
#define DOWNLINK_LOG_MAX 10
#define PUBLISH_LOG_MAX 10
#define MAX_AOA_ITEMS 10
#define SETTINGS_VERSION 3

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, "TRACE: " fmt, ## args); printf("TRACE: " fmt, ## args); }

// Global state
static GMainLoop *main_loop = NULL;
static pthread_t health_thread;
static pthread_t publish_thread;
static int running = 1;
static cJSON* eventSubscriptions = NULL;
static int g_callbacks_configured = 0;
static cJSON* g_downlink_log = NULL;
static cJSON* g_publish_log = NULL;
static pthread_mutex_t g_publish_log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Transmission settings
typedef struct {
	int human;
	int car;
	int bike;
	int bus;
	int truck;
	int other;
} ClassSelection;

typedef struct {
	char scenario[64];
	ClassSelection classes;
	char value[16];
} ScenarioPublishConfig;

static int g_counting_interval_minutes = 15;
static int g_counting_port = 10;
static ClassSelection g_counting_classes = {1, 1, 1, 1, 1, 1};
static ScenarioPublishConfig g_counting_publish[MAX_AOA_ITEMS];
static int g_counting_publish_count = 0;
static time_t g_next_counting_publish_time = 0;

static int g_occupancy_interval_minutes = 15;
static int g_occupancy_port = 0;
static char g_occupancy_value[16] = "average";
static ClassSelection g_occupancy_classes = {1, 1, 1, 1, 1, 1};
static ScenarioPublishConfig g_occupancy_publish[MAX_AOA_ITEMS];
static int g_occupancy_publish_count = 0;
static time_t g_next_occupancy_publish_time = 0;
static pthread_mutex_t g_publish_mutex = PTHREAD_MUTEX_INITIALIZER;

// Counter tracking structures
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

static CounterState g_counters[MAX_AOA_ITEMS];
static int g_counter_count = 0;
static time_t g_last_save_time = 0;
static pthread_mutex_t g_counter_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static OccupancyState g_occupancy[MAX_AOA_ITEMS];
static int g_occupancy_count = 0;
static pthread_mutex_t g_occupancy_mutex = PTHREAD_MUTEX_INITIALIZER;
static double Occupancy_Class_Value(const OccupancyState* occupancy, const char* occupancy_value, const char* class_name);
static void Update_Occupancy_ACAP_Status(void);

// Settings cache
static char g_b100_ip[64] = "192.168.0.3";
static int g_b100_port = 80;
static char g_callback_ip[64] = "192.168.0.2";
static int g_callback_port = 80;
static char g_callback_digest_user[32] = "aib100";
static char g_callback_digest_password[32] = "aib100";
static int g_health_check_interval = 60;

// App start time for uptime calculation
static time_t g_app_start_time = 0;

// Connection health-check state (all protected by g_health_mutex)
// Every 10th uplink is sent confirmed to validate the link.
// If no ACK is observed within 4 minutes, clear the pending confirmation state.
static int g_unconf_count = 0;        // counts consecutive unconfirmed uplinks
static int g_awaiting_confirm = 0;    // 1 while waiting for a confirmed ACK
static int g_conf_trial_count = 0;    // reserved for future confirmation retry policy
static time_t g_conf_sent_time = 0;   // when the last confirmed uplink was sent
static pthread_mutex_t g_health_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================================================================
// Counter Management
// ==================================================================

CounterState* Find_Or_Create_Counter(const char* scenario) {
	pthread_mutex_lock(&g_counter_mutex);
	
	// Find existing counter
	for (int i = 0; i < g_counter_count; i++) {
		if (strcmp(g_counters[i].scenario, scenario) == 0) {
			pthread_mutex_unlock(&g_counter_mutex);
			return &g_counters[i];
		}
	}
	
	// Create new counter if space available
	if (g_counter_count < MAX_AOA_ITEMS) {
		CounterState* counter = &g_counters[g_counter_count];
		memset(counter, 0, sizeof(CounterState));
		strncpy(counter->scenario, scenario, sizeof(counter->scenario) - 1);
		g_counter_count++;
		LOG("Created new counter for scenario: %s\n", scenario);
		pthread_mutex_unlock(&g_counter_mutex);
		return counter;
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
	LOG_WARN("Too many counters, cannot create for %s\n", scenario);
	return NULL;
}

OccupancyState* Find_Or_Create_Occupancy(const char* scenario) {
	pthread_mutex_lock(&g_occupancy_mutex);
	
	for (int i = 0; i < g_occupancy_count; i++) {
		if (strcmp(g_occupancy[i].scenario, scenario) == 0) {
			pthread_mutex_unlock(&g_occupancy_mutex);
			return &g_occupancy[i];
		}
	}
	
	if (g_occupancy_count < MAX_AOA_ITEMS) {
		OccupancyState* occupancy = &g_occupancy[g_occupancy_count];
		memset(occupancy, 0, sizeof(OccupancyState));
		strncpy(occupancy->scenario, scenario, sizeof(occupancy->scenario) - 1);
		g_occupancy_count++;
		LOG("Created new occupancy state for scenario: %s\n", scenario);
		pthread_mutex_unlock(&g_occupancy_mutex);
		return occupancy;
	}
	
	pthread_mutex_unlock(&g_occupancy_mutex);
	LOG_WARN("Too many occupancy scenarios, cannot create for %s\n", scenario);
	return NULL;
}

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

static int Is_Valid_Occupancy_Value(const char* value) {
	return value &&
	       (strcmp(value, "min") == 0 || strcmp(value, "max") == 0 || strcmp(value, "average") == 0);
}

static int Is_Occupancy_Scenario_Type(const char* scenario_type) {
	return scenario_type &&
	       (strcmp(scenario_type, "OccupancyInArea") == 0 || strcmp(scenario_type, "occupancyInArea") == 0);
}

static int Clamp_Publish_Interval(int minutes) {
	if (minutes < 1) return 1;
	if (minutes > 60) return 60;
	return minutes;
}

static int Clamp_Publish_Port(int port) {
	if (port < 0) return 0;
	if (port > 15) return 15;
	return port;
}

static int Class_Count(ClassSelection classes) {
	return classes.human + classes.car + classes.bike + classes.bus + classes.truck + classes.other;
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
	while (item && *count < MAX_AOA_ITEMS) {
		if (item->string && cJSON_IsObject(item)) {
			ScenarioPublishConfig* config = &configs[*count];
			memset(config, 0, sizeof(ScenarioPublishConfig));
			strncpy(config->scenario, item->string, sizeof(config->scenario) - 1);
			config->classes = default_classes;
			strncpy(config->value, default_value && Is_Valid_Occupancy_Value(default_value) ? default_value : "average", sizeof(config->value) - 1);
			Load_Class_Selection(cJSON_GetObjectItem(item, "classes"), &config->classes);
			cJSON* value = cJSON_GetObjectItem(item, "value");
			if (value && value->valuestring && Is_Valid_Occupancy_Value(value->valuestring)) {
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

static ClassSelection Counting_Classes_For_Scenario(const char* scenario) {
	ScenarioPublishConfig* config = Find_Scenario_Config(g_counting_publish, g_counting_publish_count, scenario);
	return config ? config->classes : g_counting_classes;
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

static void Update_Occupancy_ACAP_Status(void) {
	cJSON* areas = cJSON_CreateArray();
	if (!areas) return;

	pthread_mutex_lock(&g_publish_mutex);
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
		if (occupancy->has_sample) {
			Add_Selected_Occupancy_Status_Labels(labels, occupancy, publish_config);
		}
		cJSON_AddItemToObject(area, "labels", labels);
		cJSON_AddItemToArray(areas, area);
	}
	pthread_mutex_unlock(&g_occupancy_mutex);
	pthread_mutex_unlock(&g_publish_mutex);

	ACAP_STATUS_SetObject("occupancy", "areas", areas);
	cJSON_Delete(areas);
}

static void Load_Counting_Publish_Config(cJSON* config) {
	if (!config) return;
	cJSON* interval = cJSON_GetObjectItem(config, "intervalMinutes");
	if (interval) g_counting_interval_minutes = Clamp_Publish_Interval(interval->valueint);
	cJSON* port = cJSON_GetObjectItem(config, "port");
	if (port) g_counting_port = Clamp_Publish_Port(port->valueint);
	Load_Class_Selection(cJSON_GetObjectItem(config, "classes"), &g_counting_classes);
	Load_Scenario_Publish_Config(cJSON_GetObjectItem(config, "scenarios"), g_counting_publish, &g_counting_publish_count, g_counting_classes, "average");
}

static void Load_Occupancy_Publish_Config(cJSON* config) {
	if (!config) return;
	cJSON* interval = cJSON_GetObjectItem(config, "intervalMinutes");
	if (interval) g_occupancy_interval_minutes = Clamp_Publish_Interval(interval->valueint);
	cJSON* port = cJSON_GetObjectItem(config, "port");
	if (port) g_occupancy_port = Clamp_Publish_Port(port->valueint);
	cJSON* value = cJSON_GetObjectItem(config, "value");
	if (value && value->valuestring && Is_Valid_Occupancy_Value(value->valuestring)) {
		strncpy(g_occupancy_value, value->valuestring, sizeof(g_occupancy_value) - 1);
		g_occupancy_value[sizeof(g_occupancy_value) - 1] = '\0';
	}
	Load_Class_Selection(cJSON_GetObjectItem(config, "classes"), &g_occupancy_classes);
	Load_Scenario_Publish_Config(cJSON_GetObjectItem(config, "scenarios"), g_occupancy_publish, &g_occupancy_publish_count, g_occupancy_classes, g_occupancy_value);
}

static cJSON* Ensure_Object(cJSON* parent, const char* key) {
	cJSON* object = cJSON_GetObjectItem(parent, key);
	if (!object || !cJSON_IsObject(object)) {
		cJSON* replacement = cJSON_CreateObject();
		if (object) cJSON_ReplaceItemInObject(parent, key, replacement);
		else cJSON_AddItemToObject(parent, key, replacement);
		object = replacement;
	}
	return object;
}

static void Set_Number(cJSON* parent, const char* key, int value) {
	cJSON* item = cJSON_GetObjectItem(parent, key);
	if (item) cJSON_ReplaceItemInObject(parent, key, cJSON_CreateNumber(value));
	else cJSON_AddNumberToObject(parent, key, value);
}

static void Set_String(cJSON* parent, const char* key, const char* value) {
	cJSON* item = cJSON_GetObjectItem(parent, key);
	if (item) cJSON_ReplaceItemInObject(parent, key, cJSON_CreateString(value));
	else cJSON_AddStringToObject(parent, key, value);
}

static void Set_Object_Duplicate(cJSON* parent, const char* key, cJSON* source) {
	if (!source) return;
	cJSON* copy = cJSON_Duplicate(source, 1);
	if (!copy) return;
	cJSON* item = cJSON_GetObjectItem(parent, key);
	if (item) cJSON_ReplaceItemInObject(parent, key, copy);
	else cJSON_AddItemToObject(parent, key, copy);
}

static cJSON* Class_Array_JSON(ClassSelection classes) {
	cJSON* array = cJSON_CreateArray();
	if (classes.human) cJSON_AddItemToArray(array, cJSON_CreateString("human"));
	if (classes.car) cJSON_AddItemToArray(array, cJSON_CreateString("car"));
	if (classes.bike) cJSON_AddItemToArray(array, cJSON_CreateString("bike"));
	if (classes.bus) cJSON_AddItemToArray(array, cJSON_CreateString("bus"));
	if (classes.truck) cJSON_AddItemToArray(array, cJSON_CreateString("truck"));
	if (classes.other) cJSON_AddItemToArray(array, cJSON_CreateString("other"));
	return array;
}

static int Definition_Array_Has_Name(cJSON* array, const char* name) {
	if (!array || !name) return 0;
	cJSON* item = NULL;
	cJSON_ArrayForEach(item, array) {
		cJSON* item_name = cJSON_GetObjectItem(item, "name");
		if (item_name && item_name->valuestring && strcmp(item_name->valuestring, name) == 0) return 1;
	}
	return 0;
}

static void Reset_Publish_Schedule_Locked(void) {
	time_t now = time(NULL);
	g_next_counting_publish_time = g_counting_port > 0 ? now + (g_counting_interval_minutes * 60) : 0;
	g_next_occupancy_publish_time = g_occupancy_port > 0 ? now + (g_occupancy_interval_minutes * 60) : 0;
}

static void Add_Decoder_Definition(cJSON* definitions, const char* name, const char* event, int port, const char* value, int value_type_code, ClassSelection classes) {
	if (!definitions || !name) return;
	cJSON* item = cJSON_CreateObject();
	if (!item) return;
	cJSON_AddStringToObject(item, "name", name);
	if (event) cJSON_AddStringToObject(item, "event", event);
	cJSON_AddNumberToObject(item, "port", port);
	if (value) {
		cJSON_AddStringToObject(item, "value", value);
		cJSON_AddNumberToObject(item, "valueTypeCode", value_type_code);
	}
	cJSON_AddItemToObject(item, "classes", Class_Array_JSON(classes));
	cJSON_AddItemToArray(definitions, item);
}

static void Migrate_Settings(cJSON* settings) {
	if (!settings) return;

	cJSON* saved = ACAP_FILE_Read("localdata/settings.json");
	cJSON* saved_version_json = saved ? cJSON_GetObjectItem(saved, "settingsVersion") : NULL;
	int saved_version = saved_version_json ? saved_version_json->valueint : (saved ? 1 : SETTINGS_VERSION);

	if (saved_version >= SETTINGS_VERSION) {
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		cJSON* transmission = Ensure_Object(settings, "transmission");
		Ensure_Object(Ensure_Object(transmission, "counting"), "scenarios");
		Ensure_Object(Ensure_Object(transmission, "occupancy"), "scenarios");
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 2) {
		LOG("Migrating settings from version 2 to %d\n", SETTINGS_VERSION);
		cJSON* transmission = Ensure_Object(settings, "transmission");
		Ensure_Object(Ensure_Object(transmission, "counting"), "scenarios");
		Ensure_Object(Ensure_Object(transmission, "occupancy"), "scenarios");
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	LOG("Migrating settings from version %d to %d\n", saved_version, SETTINGS_VERSION);

	cJSON* transmission = Ensure_Object(settings, "transmission");
	cJSON* counting = Ensure_Object(transmission, "counting");
	cJSON* occupancy = Ensure_Object(transmission, "occupancy");
	cJSON* old_transmission = saved ? cJSON_GetObjectItem(saved, "transmission") : NULL;
	cJSON* old_lorawan = saved ? cJSON_GetObjectItem(saved, "lorawan") : NULL;

	int interval = 15;
	int enabled = 1;
	int old_port = 10;
	const char* occupancy_value = "average";
	cJSON* old_classes = NULL;

	if (old_transmission) {
		cJSON* interval_json = cJSON_GetObjectItem(old_transmission, "intervalMinutes");
		if (interval_json) interval = Clamp_Publish_Interval(interval_json->valueint);
		cJSON* enabled_json = cJSON_GetObjectItem(old_transmission, "enabled");
		if (enabled_json) enabled = cJSON_IsTrue(enabled_json);
		cJSON* old_value = cJSON_GetObjectItem(old_transmission, "occupancyValue");
		if (old_value && old_value->valuestring && Is_Valid_Occupancy_Value(old_value->valuestring)) occupancy_value = old_value->valuestring;
		old_classes = cJSON_GetObjectItem(old_transmission, "classes");
	}
	if (old_lorawan) {
		cJSON* port_json = cJSON_GetObjectItem(old_lorawan, "port");
		if (port_json) old_port = Clamp_Publish_Port(port_json->valueint);
	}

	Set_Number(counting, "intervalMinutes", interval);
	Set_Number(counting, "port", enabled ? old_port : 0);
	if (old_classes) Set_Object_Duplicate(counting, "classes", old_classes);
	Ensure_Object(counting, "scenarios");

	Set_Number(occupancy, "intervalMinutes", interval);
	Set_Number(occupancy, "port", 0);
	Set_String(occupancy, "value", occupancy_value);
	if (old_classes) Set_Object_Duplicate(occupancy, "classes", old_classes);
	Ensure_Object(occupancy, "scenarios");

	Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
	ACAP_FILE_Write("localdata/settings.json", settings);

	if (saved) cJSON_Delete(saved);
}

static void Process_Occupancy_Event(cJSON* event) {
	cJSON* scenario_type = cJSON_GetObjectItem(event, "scenarioType");
	if (!scenario_type || !scenario_type->valuestring || !Is_Occupancy_Scenario_Type(scenario_type->valuestring)) {
		return;
	}

	cJSON* scenario_name = cJSON_GetObjectItem(event, "scenario");
	if (!scenario_name || !scenario_name->valuestring) {
		LOG_WARN("OccupancyInArea event missing scenario name\n");
		return;
	}
	cJSON* event_topic = cJSON_GetObjectItem(event, "event");
	if (!event_topic || !event_topic->valuestring || !String_Ends_With(event_topic->valuestring, "EventInterval")) {
		return;
	}

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
	
	OccupancyState* occupancy = Find_Or_Create_Occupancy(scenario_name->valuestring);
	if (!occupancy) {
		return;
	}
	
	pthread_mutex_lock(&g_occupancy_mutex);
	
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
	
	LOG("Occupancy %s event end=%s: car min=%d max=%d avg=%.2f, human min=%d max=%d avg=%.2f\n",
	    occupancy->scenario,
	    occupancy->end,
	    occupancy->min_car, occupancy->max_car, occupancy->average_car,
	    occupancy->min_human, occupancy->max_human, occupancy->average_human);
	
	pthread_mutex_unlock(&g_occupancy_mutex);
	Update_Occupancy_ACAP_Status();
}

void Load_Counters_From_File() {
	pthread_mutex_lock(&g_counter_mutex);
	
	cJSON* data = ACAP_FILE_Read("localdata/counters.json");
	if (!data) {
		LOG("No saved counters found, starting fresh\n");
		pthread_mutex_unlock(&g_counter_mutex);
		return;
	}
	
	g_counter_count = 0;
	cJSON* counter_item = data->child;
	while (counter_item && g_counter_count < MAX_AOA_ITEMS) {
		CounterState* counter = &g_counters[g_counter_count];
		memset(counter, 0, sizeof(CounterState));
		
		cJSON* scenario = cJSON_GetObjectItem(counter_item, "scenario");
		if (scenario && scenario->valuestring) {
			strncpy(counter->scenario, scenario->valuestring, sizeof(counter->scenario) - 1);
		}
		
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
		
		counter->has_reference = 0;  // Will be set on first event
		
		LOG("Loaded counter %s: total=%d, human=%d, car=%d\n", 
		    counter->scenario, counter->internal_total, counter->internal_human, counter->internal_car);
		
		g_counter_count++;
		counter_item = counter_item->next;
	}
	
	cJSON_Delete(data);
	pthread_mutex_unlock(&g_counter_mutex);
	LOG("Loaded %d counters from file\n", g_counter_count);
}

void Save_Counters_To_File() {
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

void Delete_Counter_By_Scenario(const char* scenario) {
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
		
		// Shift remaining counters down
		for (int i = found_index; i < g_counter_count - 1; i++) {
			memcpy(&g_counters[i], &g_counters[i + 1], sizeof(CounterState));
		}
		
		g_counter_count--;
		
		// Clear the last slot
		memset(&g_counters[g_counter_count], 0, sizeof(CounterState));
		
		pthread_mutex_unlock(&g_counter_mutex);
		
		// Save updated counters
		Save_Counters_To_File();
	} else {
		pthread_mutex_unlock(&g_counter_mutex);
		LOG_WARN("Counter not found for deletion: %s\n", scenario);
	}
}

void Sync_Counters_With_AOA_List(cJSON* scenario_array) {
	if (!scenario_array || !cJSON_IsArray(scenario_array)) {
		LOG_WARN("Invalid scenario array for sync\n");
		return;
	}
	
	pthread_mutex_lock(&g_counter_mutex);
	
	// Build list of scenarios to keep
	int keep_count = 0;
	char* keep_list[10];
	
	cJSON* item = scenario_array->child;
	while (item && keep_count < 10) {
		if (cJSON_IsString(item) && item->valuestring) {
			keep_list[keep_count] = item->valuestring;
			keep_count++;
		}
		item = item->next;
	}
	
	// Check each counter - remove if not in keep list
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
			
			// Shift remaining counters down
			for (int j = i; j < g_counter_count - 1; j++) {
				memcpy(&g_counters[j], &g_counters[j + 1], sizeof(CounterState));
			}
			
			g_counter_count--;
			removed++;
		}
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
	
	if (removed > 0) {
		LOG("Sync: Removed %d counter(s), saving to file...\n", removed);
		Save_Counters_To_File();
		LOG("Sync: File saved with %d counters remaining\n", g_counter_count);
	} else {
		LOG("Sync: All counters match AOA configuration (%d counters)\n", g_counter_count);
	}
}

// ==================================================================
// Event Callback
// ==================================================================

void
AOA_Event_Callback(cJSON *event, void* userdata) {

	cJSON* scenarioType = cJSON_GetObjectItem(event, "scenarioType");
	if (!scenarioType || !scenarioType->valuestring) {
		return;
	}

	if (Is_Occupancy_Scenario_Type(scenarioType->valuestring)) {
		Process_Occupancy_Event(event);
		return;
	}

	// Only process CrosslineCounting events below
	if (strcmp(scenarioType->valuestring, "CrosslineCounting") != 0) {
		return;
	}
	
	// Extract scenario name
	cJSON* scenario_name = cJSON_GetObjectItem(event, "scenario");
	if (!scenario_name || !scenario_name->valuestring) {
		LOG_WARN("CrosslineCounting event missing scenario name\n");
		return;
	}
	
	// Get counter state
	CounterState* counter = Find_Or_Create_Counter(scenario_name->valuestring);
	if (!counter) {
		return;
	}
	
	pthread_mutex_lock(&g_counter_mutex);
	
	// Extract AOA counts
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
	
	// Get reason (what triggered this count)
	cJSON* reason = cJSON_GetObjectItem(event, "reason");
	const char* reason_str = reason && reason->valuestring ? reason->valuestring : "unknown";
	
	if (!counter->has_reference) {
		// First event - set reference values
		counter->aoa_reference_total = aoa_total;
		counter->aoa_reference_human = aoa_human;
		counter->aoa_reference_car = aoa_car;
		counter->aoa_reference_bike = aoa_bike;
		counter->aoa_reference_bus = aoa_bus;
		counter->aoa_reference_truck = aoa_truck;
		counter->aoa_reference_other = aoa_other;
		counter->has_reference = 1;
		
		LOG("Set reference for %s: total=%d (reason: %s)\n", 
		    counter->scenario, aoa_total, reason_str);
	} else {
		// Calculate deltas
		int delta_total = aoa_total - counter->aoa_reference_total;
		int delta_human = aoa_human - counter->aoa_reference_human;
		int delta_car = aoa_car - counter->aoa_reference_car;
		int delta_bike = aoa_bike - counter->aoa_reference_bike;
		int delta_bus = aoa_bus - counter->aoa_reference_bus;
		int delta_truck = aoa_truck - counter->aoa_reference_truck;
		int delta_other = aoa_other - counter->aoa_reference_other;
		
		// Check if AOA counters were reset (negative delta)
		if (delta_total < 0 || delta_human < 0 || delta_car < 0 || 
		    delta_bike < 0 || delta_bus < 0 || delta_truck < 0 || delta_other < 0) {
			LOG_WARN("%s: AOA counter reset detected (delta=%d), updating reference\n",
			         counter->scenario, delta_total);
			
			// Update references without incrementing internal counters
			counter->aoa_reference_total = aoa_total;
			counter->aoa_reference_human = aoa_human;
			counter->aoa_reference_car = aoa_car;
			counter->aoa_reference_bike = aoa_bike;
			counter->aoa_reference_bus = aoa_bus;
			counter->aoa_reference_truck = aoa_truck;
			counter->aoa_reference_other = aoa_other;
		} else if (delta_total > 0) {
			// Normal case - update internal counters
			counter->internal_total += delta_total;
			counter->internal_human += delta_human;
			counter->internal_car += delta_car;
			counter->internal_bike += delta_bike;
			counter->internal_bus += delta_bus;
			counter->internal_truck += delta_truck;
			counter->internal_other += delta_other;
			
			// Update references
			counter->aoa_reference_total = aoa_total;
			counter->aoa_reference_human = aoa_human;
			counter->aoa_reference_car = aoa_car;
			counter->aoa_reference_bike = aoa_bike;
			counter->aoa_reference_bus = aoa_bus;
			counter->aoa_reference_truck = aoa_truck;
			counter->aoa_reference_other = aoa_other;
			
			LOG("%s: +%d %s (internal: %d total, %d human, %d car)\n",
			    counter->scenario, delta_total, reason_str,
			    counter->internal_total, counter->internal_human, counter->internal_car);
			
			// Auto-save if it's been more than 1 minute
			time_t now = time(NULL);
			if (now - g_last_save_time >= 60) {
				pthread_mutex_unlock(&g_counter_mutex);
				Save_Counters_To_File();
				return;
			}
		}
		// If delta is 0, ignore (no change)
	}
	
	pthread_mutex_unlock(&g_counter_mutex);
}

// ==================================================================
// Settings Callback
// ==================================================================

void
Settings_Updated_Callback( const char* service, cJSON* data) {
	char* _dbg = cJSON_PrintUnformatted(data);
	if (_dbg) { LOG_TRACE("Settings_Updated_Callback [%s]: %s\n", service, _dbg); free(_dbg); }

	// The ACAP framework calls this with the individual group name as service
	// (e.g. "b100", "lorawan", "transmission", "polling") and data is the
	// group object directly — NOT the top-level settings wrapper.

	if (strcmp(service, "b100") == 0) {
		cJSON* ip = cJSON_GetObjectItem(data, "ip");
		if (ip && ip->valuestring) {
			strncpy(g_b100_ip, ip->valuestring, sizeof(g_b100_ip) - 1);
			B100_Set_IP(g_b100_ip);
		}
		cJSON* port = cJSON_GetObjectItem(data, "port");
		if (port) {
			g_b100_port = port->valueint;
			B100_Set_Port(g_b100_port);
		}
		cJSON* timeout = cJSON_GetObjectItem(data, "timeout");
		if (timeout) {
			B100_Set_Timeout(timeout->valueint);
		}
		cJSON* cbip = cJSON_GetObjectItem(data, "callbackIP");
		if (cbip && cbip->valuestring) {
			strncpy(g_callback_ip, cbip->valuestring, sizeof(g_callback_ip) - 1);
			g_callback_ip[sizeof(g_callback_ip) - 1] = '\0';
		}
		cJSON* cbport = cJSON_GetObjectItem(data, "callbackPort");
		if (cbport && cbport->valueint > 0)
			g_callback_port = cbport->valueint;
		cJSON* cbuser = cJSON_GetObjectItem(data, "callbackDigestUser");
		if (cbuser && cbuser->valuestring) {
			strncpy(g_callback_digest_user, cbuser->valuestring, sizeof(g_callback_digest_user) - 1);
			g_callback_digest_user[sizeof(g_callback_digest_user) - 1] = '\0';
		}
		cJSON* cbpass = cJSON_GetObjectItem(data, "callbackDigestPassword");
		if (cbpass && cbpass->valuestring) {
			strncpy(g_callback_digest_password, cbpass->valuestring, sizeof(g_callback_digest_password) - 1);
			g_callback_digest_password[sizeof(g_callback_digest_password) - 1] = '\0';
		}
		// Force callback re-configuration on next health cycle so the
		// new B100 IP / callback address takes effect immediately.
		g_callbacks_configured = 0;
	}

	if (strcmp(service, "transmission") == 0) {
		int refresh_occupancy_status = 0;
		pthread_mutex_lock(&g_publish_mutex);
		cJSON* counting = cJSON_GetObjectItem(data, "counting");
		cJSON* occupancy = cJSON_GetObjectItem(data, "occupancy");
		if (counting || occupancy) {
			Load_Counting_Publish_Config(counting);
			Load_Occupancy_Publish_Config(occupancy);
			refresh_occupancy_status = occupancy != NULL;
		} else {
			Load_Counting_Publish_Config(data);
			cJSON* old_enabled = cJSON_GetObjectItem(data, "enabled");
			if (old_enabled && !cJSON_IsTrue(old_enabled)) g_counting_port = 0;
			cJSON* old_value = cJSON_GetObjectItem(data, "occupancyValue");
			if (old_value && old_value->valuestring && Is_Valid_Occupancy_Value(old_value->valuestring)) {
				strncpy(g_occupancy_value, old_value->valuestring, sizeof(g_occupancy_value) - 1);
				g_occupancy_value[sizeof(g_occupancy_value) - 1] = '\0';
				refresh_occupancy_status = 1;
			}
		}
		Reset_Publish_Schedule_Locked();
		pthread_mutex_unlock(&g_publish_mutex);
		if (refresh_occupancy_status) Update_Occupancy_ACAP_Status();
	}

	if (strcmp(service, "polling") == 0) {
		cJSON* health_interval = cJSON_GetObjectItem(data, "healthCheckIntervalSeconds");
		if (health_interval) {
			g_health_check_interval = health_interval->valueint;
		}
	}
	// downlinkCommands is read directly from settings at dispatch time; no caching needed.
}

// ==================================================================
// B100 Callbacks
// ==================================================================

// Forward declaration — defined further down in this file
static int Publish_Counters_To_LoRa(void);

void
B100_Downlink_Handler(B100_Downlink* downlink) {
	if (!downlink) return;

	syslog(LOG_WARNING, "LoRa Downlink: port=%d len=%d type=%s payload=%s RSSI=%.1f SNR=%.1f fcntDown=%d",
	    downlink->port, downlink->length, downlink->payload_type,
	    downlink->payload, downlink->rssi, downlink->snr, downlink->fcntDown);
	LOG("LoRa Downlink: port=%d len=%d type=%s payload=%s RSSI=%.1f SNR=%.1f fcntDown=%d\n",
	    downlink->port, downlink->length, downlink->payload_type,
	    downlink->payload, downlink->rssi, downlink->snr, downlink->fcntDown);

	// Update status — read by downlink.html via the 'status' endpoint
	ACAP_STATUS_SetString("lorawan", "lastDownlink", downlink->payload);
	ACAP_STATUS_SetString("lorawan", "lastDownlinkType", downlink->payload_type);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkPort", downlink->port);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkLength", downlink->length);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkRSSI", downlink->rssi);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkSNR", downlink->snr);
	ACAP_STATUS_SetNumber("lorawan", "lastDownlinkFcnt", downlink->fcntDown);

	// Maintain server-side ring buffer of last DOWNLINK_LOG_MAX downlinks
	{
		cJSON* entry = cJSON_CreateObject();
		if (entry) {
			cJSON_AddStringToObject(entry, "time",         ACAP_DEVICE_Local_Time());
			cJSON_AddNumberToObject(entry, "port",         downlink->port);
			cJSON_AddNumberToObject(entry, "length",       downlink->length);
			cJSON_AddStringToObject(entry, "payload",      downlink->payload);
			cJSON_AddStringToObject(entry, "payload_type", downlink->payload_type);
			cJSON_AddNumberToObject(entry, "rssi",         downlink->rssi);
			cJSON_AddNumberToObject(entry, "snr",          downlink->snr);
			cJSON_AddNumberToObject(entry, "fcntDown",     downlink->fcntDown);
			if (!g_downlink_log)
				g_downlink_log = cJSON_CreateArray();
			cJSON_AddItemToArray(g_downlink_log, entry);
			while (cJSON_GetArraySize(g_downlink_log) > DOWNLINK_LOG_MAX)
				cJSON_DeleteItemFromArray(g_downlink_log, 0);
			ACAP_STATUS_SetObject("lorawan", "downlinks", g_downlink_log);
		}
	}

	// -----------------------------------------------------------
	// Parse payload into a byte array for command dispatch
	// -----------------------------------------------------------
	unsigned char bytes[256] = {0};
	int byte_count = 0;

	if (strcmp(downlink->payload_type, "HEX") == 0) {
		const char* hex = downlink->payload;
		int hex_len = (int)strlen(hex);
		for (int i = 0; i + 1 < hex_len && byte_count < (int)sizeof(bytes); i += 2) {
			char pair[3] = { hex[i], hex[i+1], '\0' };
			bytes[byte_count++] = (unsigned char)strtol(pair, NULL, 16);
		}
	} else {
		byte_count = (int)strlen(downlink->payload);
		if (byte_count > (int)sizeof(bytes) - 1)
			byte_count = (int)sizeof(bytes) - 1;
		memcpy(bytes, downlink->payload, byte_count);
	}

	if (byte_count == 0) return;

	int port       = downlink->port;
	int first_byte = (int)bytes[0];

	// -----------------------------------------------------------
	// Check whether this command is enabled in settings
	// -----------------------------------------------------------
	int command_enabled = 1; // default: enabled if not listed
	cJSON* settings_obj = ACAP_Get_Config("settings");
	if (settings_obj) {
		cJSON* cmd_list = cJSON_GetObjectItem(settings_obj, "downlinkCommands");
		if (cmd_list && cJSON_IsArray(cmd_list)) {
			cJSON* cmd;
			int lookup_byte = first_byte;
			cJSON_ArrayForEach(cmd, cmd_list) {
				cJSON* p = cJSON_GetObjectItem(cmd, "port");
				cJSON* b = cJSON_GetObjectItem(cmd, "byte");
				if (p && b && p->valueint == port && b->valueint == lookup_byte) {
					cJSON* e = cJSON_GetObjectItem(cmd, "enabled");
					command_enabled = (!e || cJSON_IsTrue(e)) ? 1 : 0;
					break;
				}
			}
		}
	}

	if (!command_enabled) {
		LOG("Downlink command port=%d byte=0x%02X disabled in settings — ignored\n", port, first_byte);
		return;
	}

	// -----------------------------------------------------------
	// Dispatch
	// -----------------------------------------------------------
	if (port == 10) {
		// Port 10: Actions
		switch (first_byte) {
			case 0x01:
				LOG("Downlink: Restart Bridge\n");
				B100_Restart();
				break;
			case 0x02:
				LOG("Downlink: Join Network\n");
				B100_Join_Auto();
				break;
			case 0x03:
				LOG("Downlink: Reset Counters\n");
				pthread_mutex_lock(&g_counter_mutex);
				for (int i = 0; i < g_counter_count; i++) {
					g_counters[i].internal_total = 0;
					g_counters[i].internal_human = 0;
					g_counters[i].internal_car   = 0;
					g_counters[i].internal_bike  = 0;
					g_counters[i].internal_bus   = 0;
					g_counters[i].internal_truck = 0;
					g_counters[i].internal_other = 0;
					g_counters[i].has_reference  = 0;
				}
				pthread_mutex_unlock(&g_counter_mutex);
				Save_Counters_To_File();
				sleep(1);
				Publish_Counters_To_LoRa();
				break;
			default:
				LOG_WARN("Downlink: Unknown port 10 command 0x%02X\n", first_byte);
				break;
		}

	} else if (port == 11) {
		// Port 11: Configuration (2-byte: byte[0]=command, byte[1]=value)
		if (byte_count < 2) {
			LOG_WARN("Downlink: Port 11 requires 2 bytes, got %d\n", byte_count);
		} else {
			int value = (int)bytes[1];
			switch (first_byte) {
				case 0x01: {
					int minutes = value;
					if (minutes < 5)  minutes = 5;
					if (minutes > 60) minutes = 60;
					LOG("Downlink: Set counting publish interval to %d minutes\n", minutes);
					pthread_mutex_lock(&g_publish_mutex);
					g_counting_interval_minutes = minutes;
					pthread_mutex_unlock(&g_publish_mutex);
					if (settings_obj) {
						cJSON* trans = cJSON_GetObjectItem(settings_obj, "transmission");
						if (trans) {
							cJSON* counting = cJSON_GetObjectItem(trans, "counting");
							cJSON* iv = counting ? cJSON_GetObjectItem(counting, "intervalMinutes") : cJSON_GetObjectItem(trans, "intervalMinutes");
							if (iv) { iv->valueint = minutes; iv->valuedouble = minutes; }
							ACAP_FILE_Write("localdata/settings.json", settings_obj);
						}
					}
					break;
				}
				case 0x02: {
					int dr = value;
					if (dr >= 0 && dr <= 5) {
						LOG("Downlink: Set Data Rate DR%d\n", dr);
						cJSON* p = cJSON_CreateObject();
						cJSON_AddNumberToObject(p, "data_rate", dr);
						B100_Set_Params(p);
						cJSON_Delete(p);
					} else {
						LOG_WARN("Downlink: Set Data Rate bad value %d\n", dr);
					}
					break;
				}
				case 0x03: {
					int adr = value ? 1 : 0;
					LOG("Downlink: Set ADR %s\n", adr ? "on" : "off");
					cJSON* p = cJSON_CreateObject();
					cJSON_AddNumberToObject(p, "adr_enable", adr);
					B100_Set_Params(p);
					cJSON_Delete(p);
					break;
				}
				default:
					LOG_WARN("Downlink: Unknown port 11 command 0x%02X\n", first_byte);
					break;
			}
		}

	} else if (port == 12) {
		// Port 12: Information requests
		switch (first_byte) {
			case 0x01: {
				// Camera identity + health → reply on port 5
				const char* model    = ACAP_DEVICE_Prop("model");
				const char* serial   = ACAP_DEVICE_Prop("serial");
				const char* firmware = ACAP_DEVICE_Prop("firmware");
				int uptime_hours = (int)(ACAP_DEVICE_Uptime() / 3600.0);
				int cpu_pct = (int)(ACAP_DEVICE_CPU_Average() * 100.0);
				if (cpu_pct > 99) cpu_pct = 99;
				const char* app_ver = "?";
				cJSON* mf = ACAP_Get_Config("manifest");
				if (mf) {
					cJSON* setup = cJSON_GetObjectItem(cJSON_GetObjectItem(mf, "acapPackageConf"), "setup");
					if (setup) {
						cJSON* ver = cJSON_GetObjectItem(setup, "version");
						if (ver && ver->valuestring) app_ver = ver->valuestring;
					}
				}
				char info[192] = {0};
				snprintf(info, sizeof(info), "%s,%s,%s,%dh,%d%%,%s",
				         model    ? model    : "?",
				         serial   ? serial   : "?",
				         firmware ? firmware : "?",
				         uptime_hours, cpu_pct, app_ver);
				LOG("Downlink: Camera Info reply: %s\n", info);
				if (!B100_Send(info, 5, 0))
					LOG_WARN("Downlink: Camera Info send failed: %s\n", B100_Get_Last_Error());
				break;
			}
			case 0x02: {
				// Bridge identity + LoRaWAN state → reply on port 6
				B100_Status* s = B100_Get_Status();
				char info[256] = {0};
				snprintf(info, sizeof(info), "%s/%s,%s,%s,%.0fC,R%u,%s",
				         s->hardware[0]        ? s->hardware        : "?",
				         s->hardwareVersion[0] ? s->hardwareVersion : "?",
				         s->firmwareVersion[0] ? s->firmwareVersion : "?",
				         s->powerSource[0]     ? s->powerSource     : "?",
				         s->tempC,
				         s->restartCounter,
				         s->devAddrStr[0]      ? s->devAddrStr      : "0");
				LOG("Downlink: Bridge Info reply: %s\n", info);
				if (!B100_Send(info, 6, 0))
					LOG_WARN("Downlink: Bridge Info send failed: %s\n", B100_Get_Last_Error());
				ACAP_STATUS_SetString("lorawan", "bridgeInfo", info);
				break;
			}
			case 0x03: {
				B100_Status* s = B100_Get_Status();
				char info[128] = {0};
				snprintf(info, sizeof(info), "DR%d,%dB,%.0fdBm,%.1fdB,%uup,%udn",
				         s->dataRate, s->maxPayload,
				         s->rssi, s->snr,
				         s->fcntUp, s->fcntDown);
				LOG("Downlink: Signal Quality requested: %s\n", info);
				ACAP_STATUS_SetString("lorawan", "signalQuality", info);
				break;
			}
			default:
				LOG_WARN("Downlink: Unknown port 12 command 0x%02X\n", first_byte);
				break;
		}

	} else {
		LOG_WARN("Downlink: Unhandled port %d\n", port);
	}
}

// Push bridge device-level info to the "bridge" ACAP_STATUS group.
// Called after a successful /info fetch and on every status callback.
static void
Update_Bridge_ACAP_Status(B100_Status* status) {
	if (!status) return;
	ACAP_STATUS_SetBool("bridge", "connected", status->connected == B100_CONNECTED);
	if (status->hardware[0])
		ACAP_STATUS_SetString("bridge", "hardware", status->hardware);
	if (status->hardwareVersion[0])
		ACAP_STATUS_SetString("bridge", "hardwareVersion", status->hardwareVersion);
	if (status->firmwareVersion[0])
		ACAP_STATUS_SetString("bridge", "firmwareVersion", status->firmwareVersion);
	if (status->powerSource[0])
		ACAP_STATUS_SetString("bridge", "powerSource", status->powerSource);
	if (status->ipAddr[0])
		ACAP_STATUS_SetString("bridge", "ipAddr", status->ipAddr);
	ACAP_STATUS_SetBool("bridge", "dhcpEnabled", status->dhcpEnabled);
	ACAP_STATUS_SetBool("bridge", "mqttEnabled", status->mqttEnabled);
	ACAP_STATUS_SetBool("bridge", "httpApiEnabled", status->httpApiEnabled);
	if (status->callbackStatus[0])
		ACAP_STATUS_SetString("bridge", "callbackStatus", status->callbackStatus);
	if (status->devEUI[0])
		ACAP_STATUS_SetString("bridge", "devEUI", status->devEUI);
	if (status->devAddr != 0) {
		char addr_str[16];
		snprintf(addr_str, sizeof(addr_str), "%08X", status->devAddr);
		ACAP_STATUS_SetString("bridge", "devAddr", addr_str);
	} else if (status->devAddrStr[0]) {
		ACAP_STATUS_SetString("bridge", "devAddr", status->devAddrStr);
	}
	ACAP_STATUS_SetNumber("bridge", "restartCounter", status->restartCounter);
	if (status->tempC != 0)
		ACAP_STATUS_SetNumber("bridge", "tempC", status->tempC);
	if (status->tUnix > 0)
		ACAP_STATUS_SetNumber("bridge", "tUnix", (double)status->tUnix);
	ACAP_STATUS_SetNumber("bridge", "tamper", status->tamper);
	ACAP_STATUS_SetNumber("bridge", "gpsStatus", status->gpsStatus);
}

static void
Update_Lorawan_ACAP_Status(B100_Status* status) {
	if (!status) return;
	ACAP_STATUS_SetBool("lorawan", "connected", status->connected == B100_CONNECTED);
	ACAP_STATUS_SetBool("lorawan", "joined", status->joined);
	ACAP_STATUS_SetNumber("lorawan", "statusCode", status->statusCode);
	if (status->statusText[0])
		ACAP_STATUS_SetString("lorawan", "statusText", status->statusText);
	ACAP_STATUS_SetNumber("lorawan", "fcntUp", status->fcntUp);
	ACAP_STATUS_SetNumber("lorawan", "fcntDown", status->fcntDown);
	ACAP_STATUS_SetNumber("lorawan", "rssi", status->rssi);
	ACAP_STATUS_SetNumber("lorawan", "snr", status->snr);
	ACAP_STATUS_SetNumber("lorawan", "dataRate", status->dataRate);
	ACAP_STATUS_SetNumber("lorawan", "maxPayload", status->maxPayload);
	if (status->tUnix > 0)
		ACAP_STATUS_SetNumber("lorawan", "tUnix", (double)status->tUnix);
	ACAP_STATUS_SetNumber("lorawan", "nextUploadMs", (double)status->nextUploadMs);
	if (status->receiveTUnix > 0)
		ACAP_STATUS_SetNumber("lorawan", "downlinkTUnix", (double)status->receiveTUnix);
	ACAP_STATUS_SetNumber("lorawan", "margin", status->margin);
	ACAP_STATUS_SetNumber("lorawan", "gwCount", status->gwCount);
	if (status->devAddr != 0) {
		char addr_str[16];
		snprintf(addr_str, sizeof(addr_str), "%08X", status->devAddr);
		ACAP_STATUS_SetString("lorawan", "devAddr", addr_str);
	} else if (status->devAddrStr[0]) {
		ACAP_STATUS_SetString("lorawan", "devAddr", status->devAddrStr);
	}
}

void
B100_Status_Handler(B100_Status* status) {
	if (!status) return;

	Update_Lorawan_ACAP_Status(status);

	// Bridge device info (from /info) — separate group
	Update_Bridge_ACAP_Status(status);
	ACAP_STATUS_SetBool("bridge", "callbacksActive", g_callbacks_configured);
}

static int
Configure_B100_Callbacks(void) {
	const char* cam_ip = g_callback_ip[0] ? g_callback_ip : ACAP_DEVICE_Prop("IPv4");
	char status_uri[64], receive_uri[64], gps_uri[64];
	snprintf(status_uri, sizeof(status_uri), "/local/%s/b100_status", APP_PACKAGE);
	snprintf(receive_uri, sizeof(receive_uri), "/local/%s/b100_receive", APP_PACKAGE);
	snprintf(gps_uri, sizeof(gps_uri), "/local/%s/b100_gps", APP_PACKAGE);

	if (!B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri,
	                             g_callback_digest_user, g_callback_digest_password)) {
		g_callbacks_configured = 0;
		return 0;
	}

	g_callbacks_configured = 1;
	B100_Configure_GPS_Callback(gps_uri, 60);
	ACAP_STATUS_SetString("app", "status", "Running");
	ACAP_STATUS_SetBool("bridge", "callbacksActive", 1);
	return 1;
}

// ==================================================================
// Background Threads
// ==================================================================

void*
Health_Monitor_Thread(void* arg) {
	LOG("Health monitor thread started (interval: %ds)\n", g_health_check_interval);

	while (running) {
		// Refresh device info periodically via /info (always available)
		B100_Fetch_Device_Info();
		B100_Status* status = B100_Get_Status();
		int callback_failed = strcmp(status->callbackStatus, "fail") == 0;
		time_t now = time(NULL);
		int stale_threshold = g_health_check_interval * 2;
		if (stale_threshold < 180) stale_threshold = 180;
		int callback_stale = status->timestamp == 0 || (now - (time_t)status->timestamp) > stale_threshold;

		if (status->connected != B100_CONNECTED) {
			LOG_WARN("Health check: B100 not reachable\n");
			ACAP_STATUS_SetBool("bridge", "connected", 0);
			ACAP_STATUS_SetBool("lorawan", "connected", 0);
			ACAP_STATUS_SetString("app", "status", "B100 connection error");
			goto sleep_and_continue;
		}

		// Push device info to bridge status group, and all available lorawan stats
		Update_Bridge_ACAP_Status(status);
		Update_Lorawan_ACAP_Status(status);
		ACAP_STATUS_SetBool("bridge", "callbacksActive", g_callbacks_configured);
		ACAP_STATUS_SetBool("lorawan", "connected", 1);

		// If callbacks are not yet configured, if B100 reports http_api_enable=0,
		// or if callback delivery has failed and no callback has arrived recently,
		// reapply the callback configuration.
		if (!g_callbacks_configured || !status->httpApiEnabled || (callback_failed && callback_stale)) {
			if (g_callbacks_configured && !status->httpApiEnabled)
				LOG_WARN("B100 http_api_enable is 0 — reapplying callback configuration\n");
			if (callback_failed && callback_stale)
				LOG_WARN("B100 callback delivery failed — reapplying callback configuration\n");
			if (!Configure_B100_Callbacks()) {
				LOG_WARN("Failed to configure B100 callbacks\n");
			}
		}

		// Poll current GPS data and update ACAP status (also fires the GPS handler)
		{
			cJSON* gps_json = B100_Get_GPS();
			if (gps_json) cJSON_Delete(gps_json);
		}

		// Trigger async status request (response comes via callback)
		if (g_callbacks_configured) {
			if (!B100_Request_Status()) {
				LOG_WARN("Health check: status request failed\n");
			}
		}

		// Device restart detected → trigger immediate rejoin
		if (status->statusCode == B100_STATUS_RESTARTED ||
		    status->statusCode == B100_STATUS_AUTOJOIN_ENABLED) {
			LOG("Device restart detected (status %d), triggering auto-join...\n", status->statusCode);
			ACAP_STATUS_SetString("lorawan", "statusText", "Device restarted - rejoining");
			B100_Join_Auto();
			pthread_mutex_lock(&g_health_mutex);
			g_unconf_count = 0;
			g_awaiting_confirm = 0;
			g_conf_trial_count = 0;
			pthread_mutex_unlock(&g_health_mutex);
			goto sleep_and_continue;
		}

		// Confirmed delivery (status 10) → clear health-check state
		if (status->statusCode == B100_STATUS_SENT_CONFIRMED) {
			pthread_mutex_lock(&g_health_mutex);
			if (g_awaiting_confirm) {
				LOG("LoRa: Confirmed delivery received, link healthy\n");
				g_awaiting_confirm = 0;
				g_conf_trial_count = 0;
			}
			pthread_mutex_unlock(&g_health_mutex);
		}

		// Auto-join if not on network
		if (!status->joined) {
			cJSON* settings = ACAP_Get_Config("settings");
			if (settings) {
				cJSON* lorawan = cJSON_GetObjectItem(settings, "lorawan");
				if (lorawan) {
					cJSON* autoJoin = cJSON_GetObjectItem(lorawan, "autoJoin");
					if (autoJoin && cJSON_IsTrue(autoJoin)) {
						LOG("Auto-join: Device not joined, attempting join...\n");
						B100_Join_Auto();
					}
				}
			}
			goto sleep_and_continue;
		}

		// Confirmation timeout: do not send probe traffic; just stop waiting for this ACK.
		pthread_mutex_lock(&g_health_mutex);
		int waiting = g_awaiting_confirm;
		time_t sent = g_conf_sent_time;
		pthread_mutex_unlock(&g_health_mutex);

		if (waiting && (time(NULL) - sent) > 240) {
			LOG("LoRa: Confirmed uplink ACK not observed within timeout\n");
			pthread_mutex_lock(&g_health_mutex);
			g_awaiting_confirm = 0;
			g_conf_trial_count = 0;
			pthread_mutex_unlock(&g_health_mutex);
		}

	sleep_and_continue:
		for (int i = 0; i < g_health_check_interval && running; i++) {
			sleep(1);
		}
	}

	LOG("Health monitor thread stopped\n");
	return NULL;
}

// Downlink poller removed — downlinks now arrive via B100 receive callback

// ==================================================================
// HTTP Endpoints
// ==================================================================

void
HTTP_Endpoint_join(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	LOG("Manual join requested\n");
	
	// Get optional parameters
	char* drJoin_str = ACAP_HTTP_Request_Param(request, "drJoin");
	char* adr_str = ACAP_HTTP_Request_Param(request, "adr");
	char* drUp_str = ACAP_HTTP_Request_Param(request, "drUp");
	
	int drJoin = drJoin_str ? atoi(drJoin_str) : 0;
	int adr = adr_str ? atoi(adr_str) : 1;
	int drUp = drUp_str ? atoi(drUp_str) : 4;
	
	free(drJoin_str);
	free(adr_str);
	free(drUp_str);
	
	// Attempt join
	if (B100_Join(drJoin, adr, drUp)) {
		ACAP_HTTP_Respond_Text(response, "Join request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

void
HTTP_Endpoint_test(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	LOG("Connection test requested\n");
	
	if (B100_Test_Connection()) {
		ACAP_HTTP_Respond_Text(response, "Connection successful");
	} else {
		ACAP_HTTP_Respond_Error(response, 503, "Connection failed");
	}
}

void
HTTP_Endpoint_restart(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	LOG("B100 restart requested\n");
	
	if (B100_Restart()) {
		ACAP_HTTP_Respond_Text(response, "Restart initiated");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, "Restart failed");
	}
}

void
HTTP_Endpoint_send(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
    const char* method = ACAP_HTTP_Get_Method(request);
    if (!method || strcmp(method, "POST") != 0) {
        ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
        return;
    }
	
	// Get parameters
	char* payload = ACAP_HTTP_Request_Param(request, "payload");
	char* port_str = ACAP_HTTP_Request_Param(request, "port");
	char* confirmed_str = ACAP_HTTP_Request_Param(request, "confirmed");
	
	if (!payload) {
		free(port_str);
		free(confirmed_str);
		ACAP_HTTP_Respond_Error(response, 400, "Missing payload parameter");
		return;
	}
	
	int port = port_str ? atoi(port_str) : 10;
	int confirmed = confirmed_str ? atoi(confirmed_str) : 0;
	
	free(port_str);
	free(confirmed_str);
	
	LOG("Sending test message on port %d: %s\n", port, payload);
	
	// Send message
	if (B100_Send(payload, port, confirmed)) {
		ACAP_HTTP_Respond_Text(response, "Message sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
	
	free(payload);
}

void
HTTP_Endpoint_counters(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	pthread_mutex_lock(&g_counter_mutex);
	
	cJSON* root = cJSON_CreateObject();
	cJSON* counters_array = cJSON_CreateArray();
	
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

	pthread_mutex_lock(&g_occupancy_mutex);
	cJSON* occupancy_array = cJSON_CreateArray();
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
	
	// Add publishing info
	pthread_mutex_lock(&g_publish_mutex);
	cJSON_AddItemToObject(root, "counters", counters_array);
	cJSON_AddItemToObject(root, "occupancy", occupancy_array);
	time_t now = time(NULL);
	cJSON* publish = cJSON_CreateObject();
	cJSON* counting = cJSON_CreateObject();
	cJSON* occupancy = cJSON_CreateObject();
	cJSON_AddNumberToObject(counting, "port", g_counting_port);
	cJSON_AddNumberToObject(counting, "intervalMinutes", g_counting_interval_minutes);
	cJSON_AddNumberToObject(counting, "nextPublishTime", (double)g_next_counting_publish_time);
	cJSON_AddNumberToObject(counting, "secondsUntilPublish", g_counting_port > 0 && g_next_counting_publish_time > now ? (int)(g_next_counting_publish_time - now) : 0);
	cJSON_AddNumberToObject(occupancy, "port", g_occupancy_port);
	cJSON_AddNumberToObject(occupancy, "intervalMinutes", g_occupancy_interval_minutes);
	cJSON_AddStringToObject(occupancy, "value", g_occupancy_value);
	cJSON_AddNumberToObject(occupancy, "nextPublishTime", (double)g_next_occupancy_publish_time);
	cJSON_AddNumberToObject(occupancy, "secondsUntilPublish", g_occupancy_port > 0 && g_next_occupancy_publish_time > now ? (int)(g_next_occupancy_publish_time - now) : 0);
	cJSON_AddItemToObject(publish, "counting", counting);
	cJSON_AddItemToObject(publish, "occupancy", occupancy);
	cJSON_AddItemToObject(root, "publish", publish);
	pthread_mutex_unlock(&g_publish_mutex);

	pthread_mutex_lock(&g_publish_log_mutex);
	cJSON* publish_log = g_publish_log ? cJSON_Duplicate(g_publish_log, 1) : cJSON_CreateArray();
	cJSON_AddItemToObject(root, "publishLog", publish_log ? publish_log : cJSON_CreateArray());
	pthread_mutex_unlock(&g_publish_log_mutex);
	
	ACAP_HTTP_Respond_JSON(response, root);
	cJSON_Delete(root);
}

// ==================================================================
// LoRa Publishing — sends binary counter data directly via byte array
// ==================================================================

static void Append_U16(unsigned char* buffer, size_t* offset, uint16_t value) {
	buffer[(*offset)++] = value & 0xFF;
	buffer[(*offset)++] = (value >> 8) & 0xFF;
}

static void Append_U8(unsigned char* buffer, size_t* offset, uint8_t value) {
	buffer[(*offset)++] = value;
}

static char* Bytes_To_Hex_String(const unsigned char* data, size_t length) {
	if (!data && length > 0) return NULL;
	char* hex = malloc((length * 2) + 1);
	if (!hex) return NULL;
	for (size_t i = 0; i < length; i++) {
		snprintf(hex + (i * 2), 3, "%02X", data[i]);
	}
	hex[length * 2] = '\0';
	return hex;
}

static void Record_Publish_Log(int port, const unsigned char* data, size_t length) {
	char* hex = Bytes_To_Hex_String(data, length);
	if (!hex) return;

	time_t now = time(NULL);
	struct tm local_time;
	localtime_r(&now, &local_time);
	char date_text[16];
	char time_text[16];
	strftime(date_text, sizeof(date_text), "%Y-%m-%d", &local_time);
	strftime(time_text, sizeof(time_text), "%H:%M:%S", &local_time);

	cJSON* entry = cJSON_CreateObject();
	if (!entry) {
		free(hex);
		return;
	}
	cJSON_AddStringToObject(entry, "date", date_text);
	cJSON_AddStringToObject(entry, "time", time_text);
	cJSON_AddNumberToObject(entry, "port", port);
	cJSON_AddStringToObject(entry, "payload", hex);
	free(hex);

	pthread_mutex_lock(&g_publish_log_mutex);
	if (!g_publish_log) g_publish_log = cJSON_CreateArray();
	if (!g_publish_log) {
		pthread_mutex_unlock(&g_publish_log_mutex);
		cJSON_Delete(entry);
		return;
	}
	cJSON_InsertItemInArray(g_publish_log, 0, entry);
	while (cJSON_GetArraySize(g_publish_log) > PUBLISH_LOG_MAX) {
		cJSON_DeleteItemFromArray(g_publish_log, cJSON_GetArraySize(g_publish_log) - 1);
	}
	pthread_mutex_unlock(&g_publish_log_mutex);
}

static uint16_t Wrap_U16_Int(int value) {
	if (value < 0) return 0;
	return (uint16_t)(value & 0xFFFF);
}

static uint8_t Encode_Occupancy_U8(double value) {
	double rounded = round(value);
	if (rounded < 0.0) return 0;
	if (rounded > 255.0) return 255;
	return (uint8_t)rounded;
}

static uint8_t Occupancy_Value_Type_Code(const char* value_type) {
	if (value_type && strcmp(value_type, "min") == 0) return 1;
	if (value_type && strcmp(value_type, "average") == 0) return 2;
	return 0;
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

static int Next_Confirmed_Flag(void) {
	int use_confirmed = 0;
	pthread_mutex_lock(&g_health_mutex);
	g_unconf_count++;
	use_confirmed = (g_unconf_count >= 10) ? 1 : 0;
	if (use_confirmed) {
		g_unconf_count = 0;
		g_awaiting_confirm = 1;
		g_conf_trial_count = 0;
		g_conf_sent_time = time(NULL);
		LOG("LoRa: Sending health-check confirmed uplink\n");
	}
	pthread_mutex_unlock(&g_health_mutex);
	return use_confirmed;
}

int Publish_Counters_To_LoRa() {
	pthread_mutex_lock(&g_publish_mutex);
	int port = g_counting_port;
	pthread_mutex_unlock(&g_publish_mutex);
	
	if (port == 0) {
		LOG("Counting publish disabled (port 0)\n");
		return 0;
	}
	
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
			pthread_mutex_unlock(&g_counter_mutex);
			LOG_WARN("%s: No current AOA counter values available yet; skipping counting publish\n", g_counters[i].scenario);
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
	
	int use_confirmed = Next_Confirmed_Flag();
	LOG("LoRa: Publishing %zu counting bytes (%d counters, %d selected classes) on port %d%s\n",
	    buffer_size, published_counter_count, total_class_count, port, use_confirmed ? " [confirmed]" : "");

	int success = B100_Send_Bytes(buffer, (int)buffer_size, port, use_confirmed);
	if (success) {
		LOG("LoRa: Counting publish accepted\n");
		Record_Publish_Log(port, buffer, buffer_size);
		pthread_mutex_lock(&g_publish_mutex);
		g_next_counting_publish_time = time(NULL) + (g_counting_interval_minutes * 60);
		pthread_mutex_unlock(&g_publish_mutex);
	} else {
		LOG_WARN("LoRa: Counting publish failed - %s\n", B100_Get_Last_Error());
	}
	
	free(buffer);
	return success;
}

static int Publish_Occupancy_To_LoRa() {
	pthread_mutex_lock(&g_publish_mutex);
	int port = g_occupancy_port;
	pthread_mutex_unlock(&g_publish_mutex);
	
	if (port == 0) {
		LOG("Occupancy publish disabled (port 0)\n");
		return 0;
	}
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
			Append_U8(buffer, &offset, Encode_Occupancy_U8(value && cJSON_IsNumber(value) ? value->valuedouble : 0.0));
			written++;
		}
	}
	cJSON_Delete(areas);
	
	int use_confirmed = Next_Confirmed_Flag();
	LOG("LoRa: Publishing %zu occupancy bytes (%d scenarios, %d selected labels) on port %d%s\n",
	    buffer_size, occupancy_sample_count, total_class_count, port, use_confirmed ? " [confirmed]" : "");

	int success = B100_Send_Bytes(buffer, (int)buffer_size, port, use_confirmed);
	
	if (success) {
		LOG("LoRa: Occupancy publish accepted\n");
		Record_Publish_Log(port, buffer, buffer_size);
		pthread_mutex_lock(&g_publish_mutex);
		g_next_occupancy_publish_time = time(NULL) + (g_occupancy_interval_minutes * 60);
		pthread_mutex_unlock(&g_publish_mutex);
	} else {
		LOG_WARN("LoRa: Occupancy publish failed - %s\n", B100_Get_Last_Error());
	}
	
	free(buffer);
	return success;
}

void*
Publish_Thread(void* arg) {
	LOG("Publish thread started\n");
	
	// Set initial next publish times
	pthread_mutex_lock(&g_publish_mutex);
	Reset_Publish_Schedule_Locked();
	pthread_mutex_unlock(&g_publish_mutex);
	
	while (running) {
		pthread_mutex_lock(&g_publish_mutex);
		int counting_enabled = g_counting_port > 0;
		int occupancy_enabled = g_occupancy_port > 0;
		time_t next_counting_time = g_next_counting_publish_time;
		time_t next_occupancy_time = g_next_occupancy_publish_time;
		pthread_mutex_unlock(&g_publish_mutex);
		
		time_t now = time(NULL);
		if (counting_enabled && now >= next_counting_time) {
			Publish_Counters_To_LoRa();
		}
		if (occupancy_enabled && now >= next_occupancy_time) {
			Publish_Occupancy_To_LoRa();
		}
		
		// Sleep for 10 seconds before checking again
		for (int i = 0; i < 10 && running; i++) {
			sleep(1);
		}
	}
	
	LOG("Publish thread stopped\n");
	return NULL;
}

void
HTTP_Endpoint_publish(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	LOG("Manual publish requested\n");
	char* stream = ACAP_HTTP_Request_Param(request, "stream");
	int request_counting = !stream || strcmp(stream, "counting") == 0 || strcmp(stream, "all") == 0;
	int request_occupancy = !stream || strcmp(stream, "occupancy") == 0 || strcmp(stream, "all") == 0;
	if (!request_counting && !request_occupancy) {
		if (stream) free(stream);
		ACAP_HTTP_Respond_Error(response, 400, "Invalid publish stream");
		return;
	}

	int attempted = 0;
	int success = 0;
	pthread_mutex_lock(&g_publish_mutex);
	int counting_enabled = g_counting_port > 0;
	int occupancy_enabled = g_occupancy_port > 0;
	pthread_mutex_unlock(&g_publish_mutex);

	if (request_counting && counting_enabled) {
		attempted++;
		if (Publish_Counters_To_LoRa()) success++;
	}
	if (request_occupancy && occupancy_enabled) {
		attempted++;
		if (Publish_Occupancy_To_LoRa()) success++;
	}
	if (stream) free(stream);
	
	if (attempted > 0 && success > 0) {
		ACAP_HTTP_Respond_Text(response, "Publish request sent");
	} else if (attempted == 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Selected publish stream is disabled");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to publish selected stream");
	}
}

void
HTTP_Endpoint_receive(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	// Manual linkcheck trigger
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	LOG("Manual linkcheck requested\n");
	if (!Configure_B100_Callbacks()) {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to configure B100 callbacks");
		return;
	}
	
	if (B100_Link_Check()) {
		ACAP_HTTP_Respond_Text(response, "Linkcheck request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

void
HTTP_Endpoint_b100_request_status(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	// Ensure B100 has http_api_enable=1 and correct callback URIs configured
	if (!Configure_B100_Callbacks()) {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to configure B100 callbacks");
		return;
	}

	if (B100_Request_Status()) {
		ACAP_HTTP_Respond_Text(response, "Status request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

// ==================================================================
// B100 Callback Endpoints — the B100 POSTs status/downlink data here
// ==================================================================

void
HTTP_Endpoint_B100_Status_Callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}

	char *json_str = cJSON_PrintUnformatted(body);
	if( json_str ) {
		LOG_TRACE("B100 status callback received JSON: %s\n", json_str);
		free(json_str);
	}

	B100_Process_Status_Callback(body);
	cJSON_Delete(body);

	ACAP_HTTP_Respond_Text(response, "OK");
}

void
HTTP_Endpoint_B100_Receive_Callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		LOG_WARN("B100 receive callback: failed to parse JSON body (Content-Type may be wrong)\n");
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}

	char *json_str = cJSON_PrintUnformatted(body);
	if (json_str) {
		syslog(LOG_WARNING, "B100 receive callback: %s", json_str);
		LOG("B100 receive callback: %s\n", json_str);
		free(json_str);
	}

	B100_Process_Receive_Callback(body);
	cJSON_Delete(body);

	ACAP_HTTP_Respond_Text(response, "OK");
}

void
B100_GPS_Handler(B100_GPS* gps) {
	if (!gps) return;

	ACAP_STATUS_SetNumber("gps", "gps_status", gps->gps_status);
	ACAP_STATUS_SetString("gps", "ns",          gps->ns);
	ACAP_STATUS_SetNumber("gps", "lat",          gps->lat);
	ACAP_STATUS_SetString("gps", "ew",          gps->ew);
	ACAP_STATUS_SetNumber("gps", "lon",          gps->lon);
	ACAP_STATUS_SetNumber("gps", "alt",          gps->alt);
	ACAP_STATUS_SetNumber("gps", "nosv",         gps->nosv);
	ACAP_STATUS_SetNumber("gps", "pdop",         gps->pdop);
	ACAP_STATUS_SetNumber("gps", "hdop",         gps->hdop);
	ACAP_STATUS_SetNumber("gps", "vdop",         gps->vdop);
	ACAP_STATUS_SetString("gps", "utc",          gps->utc);
	ACAP_STATUS_SetString("gps", "date",         gps->date);
	ACAP_STATUS_SetNumber("gps", "sog",          gps->sog);
	ACAP_STATUS_SetNumber("gps", "cog",          gps->cog);
}

void
HTTP_Endpoint_B100_GPS_Callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}

	char* json_str = cJSON_PrintUnformatted(body);
	if (json_str) {
		LOG_TRACE("B100 GPS callback received JSON: %s\n", json_str);
		free(json_str);
	}

	B100_Process_GPS_Callback(body);
	cJSON_Delete(body);

	ACAP_HTTP_Respond_Text(response, "OK");
}

void
HTTP_Endpoint_gps(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* gps_json = B100_Get_GPS();
	if (gps_json) {
		ACAP_HTTP_Respond_JSON(response, gps_json);
		cJSON_Delete(gps_json);
	} else {
		ACAP_HTTP_Respond_Error(response, 503, "B100 GPS not available");
	}
}

// ==================================================================
// B100 Info/Config Endpoint — expose B100 device info and params
// ==================================================================

void
HTTP_Endpoint_b100_info(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	B100_Fetch_Device_Info();
	B100_Status* status = B100_Get_Status();
	Update_Bridge_ACAP_Status(status);
	Update_Lorawan_ACAP_Status(status);
	ACAP_STATUS_SetBool("bridge", "callbacksActive", g_callbacks_configured);

	cJSON* info = B100_Get_Info();
	if (info) {
		// Add current callback config
		cJSON* cb_params = B100_Get_Params("callback_addr");
		if (cb_params) {
			cJSON* addr = cJSON_GetObjectItem(cb_params, "callback_addr");
			if (addr && addr->valuestring)
				cJSON_AddStringToObject(info, "callback_configured_addr", addr->valuestring);
			cJSON_Delete(cb_params);
		}
		cJSON_AddBoolToObject(info, "callbacks_active", g_callbacks_configured);
		
		ACAP_HTTP_Respond_JSON(response, info);
		cJSON_Delete(info);
	} else {
		ACAP_HTTP_Respond_Error(response, 503, "B100 not reachable");
	}
}

void
HTTP_Endpoint_b100_params(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	
	if (method && strcmp(method, "GET") == 0) {
		// Read all parameters
		cJSON* params = B100_Get_Params(NULL);
		if (params) {
			ACAP_HTTP_Respond_JSON(response, params);
			cJSON_Delete(params);
		} else {
			ACAP_HTTP_Respond_Error(response, 503, "B100 not reachable");
		}
	} else if (method && strcmp(method, "POST") == 0) {
		// Set parameters
		cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
		if (!body) {
			ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
			return;
		}
		if (B100_Set_Params(body)) {
			cJSON_Delete(body);
			ACAP_HTTP_Respond_Text(response, "Parameters updated");
		} else {
			cJSON_Delete(body);
			ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
		}
	} else {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be GET or POST");
	}
}

void
HTTP_Endpoint_linkcheck(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	if (!Configure_B100_Callbacks()) {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to configure B100 callbacks");
		return;
	}
	
	if (B100_Link_Check()) {
		ACAP_HTTP_Respond_Text(response, "Linkcheck request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

void
HTTP_Endpoint_delete_counter(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	// Get scenario parameter
	char* scenario = ACAP_HTTP_Request_Param(request, "scenario");
	if (!scenario) {
		ACAP_HTTP_Respond_Error(response, 400, "Missing scenario parameter");
		return;
	}
	
	LOG("Delete counter requested: %s\n", scenario);
	Delete_Counter_By_Scenario(scenario);
	free(scenario);
	
	ACAP_HTTP_Respond_Text(response, "Counter deleted");
}

void
HTTP_Endpoint_sync_counters(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	
	// Get JSON body with scenario list
	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}
	
	cJSON* scenarios = cJSON_GetObjectItem(body, "scenarios");
	if (!scenarios || !cJSON_IsArray(scenarios)) {
		cJSON_Delete(body);
		ACAP_HTTP_Respond_Error(response, 400, "Missing scenarios array");
		return;
	}
	
	int scenario_count = cJSON_GetArraySize(scenarios);
	LOG("Sync counters requested with %d AOA scenarios\n", scenario_count);
	LOG("Current backend has %d counters before sync\n", g_counter_count);
	
	Sync_Counters_With_AOA_List(scenarios);
	
	cJSON_Delete(body);
	
	// Return current counter count
	cJSON* result = cJSON_CreateObject();
	cJSON_AddNumberToObject(result, "removed", -1);  // Will be calculated in sync
	cJSON_AddNumberToObject(result, "remaining", g_counter_count);
	cJSON_AddStringToObject(result, "status", "synchronized");
	
	ACAP_HTTP_Respond_JSON(response, result);
	cJSON_Delete(result);
}

void
HTTP_Endpoint_set_counters(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		ACAP_HTTP_Respond_Error(response, 400, "Invalid JSON body");
		return;
	}

	cJSON* scenario_json = cJSON_GetObjectItem(body, "scenario");
	if (!scenario_json || !scenario_json->valuestring || strlen(scenario_json->valuestring) == 0) {
		cJSON_Delete(body);
		ACAP_HTTP_Respond_Error(response, 400, "Missing scenario name");
		return;
	}

	const char* scenario = scenario_json->valuestring;

	pthread_mutex_lock(&g_counter_mutex);

	// Find the counter for this scenario
	CounterState* counter = NULL;
	for (int i = 0; i < g_counter_count; i++) {
		if (strcmp(g_counters[i].scenario, scenario) == 0) {
			counter = &g_counters[i];
			break;
		}
	}

	if (!counter) {
		pthread_mutex_unlock(&g_counter_mutex);
		cJSON_Delete(body);
		ACAP_HTTP_Respond_Error(response, 404, "Scenario not found");
		return;
	}

	// Update counter values from request
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

	// Recalculate total
	counter->internal_total = counter->internal_human + counter->internal_car +
	                          counter->internal_bike + counter->internal_bus +
	                          counter->internal_truck + counter->internal_other;

	// Reset AOA reference so next event calculates delta from current AOA value
	counter->has_reference = 0;

	LOG("Set counters for %s: human=%d car=%d bike=%d bus=%d truck=%d other=%d total=%d\n",
	    scenario, counter->internal_human, counter->internal_car, counter->internal_bike,
	    counter->internal_bus, counter->internal_truck, counter->internal_other,
	    counter->internal_total);

	pthread_mutex_unlock(&g_counter_mutex);
	cJSON_Delete(body);

	// Persist to file
	Save_Counters_To_File();

	ACAP_HTTP_Respond_Text(response, "Counter values updated");
}

void
HTTP_Endpoint_translator(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	(void)request;

	pthread_mutex_lock(&g_publish_mutex);
	pthread_mutex_lock(&g_counter_mutex);
	pthread_mutex_lock(&g_occupancy_mutex);
	
	int counting_port = g_counting_port;
	int occupancy_port = g_occupancy_port;
	cJSON* counter_defs = cJSON_CreateArray();
	cJSON* occupancy_defs = cJSON_CreateArray();
	
	for (int i = 0; i < g_counter_count; i++) {
		ClassSelection classes = Counting_Classes_For_Scenario(g_counters[i].scenario);
		Add_Decoder_Definition(counter_defs, g_counters[i].scenario, NULL, counting_port, NULL, 0, classes);
	}

	for (int i = 0; i < g_occupancy_count; i++) {
		ScenarioPublishConfig publish_config = Occupancy_Config_For_Scenario(g_occupancy[i].scenario);
		Add_Decoder_Definition(occupancy_defs, g_occupancy[i].scenario, g_occupancy[i].event_topic, occupancy_port, publish_config.value, Occupancy_Value_Type_Code(publish_config.value), publish_config.classes);
	}

	for (int i = 0; i < g_occupancy_publish_count; i++) {
		ScenarioPublishConfig* publish_config = &g_occupancy_publish[i];
		if (Definition_Array_Has_Name(occupancy_defs, publish_config->scenario)) continue;
		Add_Decoder_Definition(occupancy_defs, publish_config->scenario, "", occupancy_port, publish_config->value, Occupancy_Value_Type_Code(publish_config->value), publish_config->classes);
	}
	char* js_counter_defs = cJSON_PrintUnformatted(counter_defs);
	char* js_occupancy_defs = cJSON_PrintUnformatted(occupancy_defs);
	if (!js_counter_defs) js_counter_defs = strdup("[]");
	if (!js_occupancy_defs) js_occupancy_defs = strdup("[]");

	char js[24576];
	int pos = 0;
	
	const char* dev_model  = ACAP_DEVICE_Prop("model")  ? ACAP_DEVICE_Prop("model")  : "unknown";
	const char* dev_serial = ACAP_DEVICE_Prop("serial") ? ACAP_DEVICE_Prop("serial") : "000000";
	const char* dev_date   = ACAP_DEVICE_Date();

	pos += snprintf(js + pos, sizeof(js) - pos,
		"/**\n"
		" * AI-B100 JavaScript Decoder\n"
		" * Device  : %s  (serial %s)\n"
		" * Generated: %s\n"
		" * Counting port: %d\n"
		" * Occupancy port: %d\n"
		" *\n"
		" * This decoder is generated from the current app settings. Download a new decoder after changing ports, labels, areas, or value types.\n"
		" */\n\n",
		dev_model, dev_serial, dev_date,
		counting_port, occupancy_port);

	pos += snprintf(js + pos, sizeof(js) - pos,
		"function safeKey(name) {\n"
		"  var key = String(name == null ? '' : name).trim().replace(/[^A-Za-z0-9_]+/g, '_').replace(/^_+|_+$/g, '');\n"
		"  if (!key) key = 'unnamed';\n"
		"  if (/^[0-9]/.test(key)) key = '_' + key;\n"
		"  return key;\n"
		"}\n\n"
		"function normalizeType(value) {\n"
		"  return value === 'average' ? 'avg' : value;\n"
		"}\n\n"
		"function readU16(bytes, offset) {\n"
		"  return (bytes[offset] | (bytes[offset + 1] << 8)) >>> 0;\n"
		"}\n\n"
		"var countingPort = %d;\n"
		"var counterScenarios = %s;\n"
		"var occupancyPort = %d;\n"
		"var occupancyScenarios = %s;\n"
		"var occupancyValueTypes = { 0: 'max', 1: 'min', 2: 'avg' };\n\n",
		counting_port, js_counter_defs, occupancy_port, js_occupancy_defs);

	pos += snprintf(js + pos, sizeof(js) - pos,
			"/**\n"
			" * Counting decoder\n"
			" *\n"
			" * Input:\n"
			" *   bytes - LoRaWAN payload bytes from the Counting port.\n"
			" *\n"
			" * Buffer data structure:\n"
			" *   The payload contains one uint16 little-endian value per selected label.\n"
			" *   Labels are encoded in this fixed order when enabled: human, car, bike, bus, truck, other.\n"
			" *   Values are repeated per configured counter in the order shown in counterScenarios below.\n"
			" *   Each value is the current AOA accumulated counter value modulo 65536.\n"
			" *   Consumers that maintain continuous counters must detect uint16 wrap-around and add the wrapped delta.\n"
			" *   Immediately after an app/camera restart, Counting publish waits until fresh AOA counter values have been received.\n"
			" *\n"
			" * Output:\n"
			" *   An object keyed by sanitized counter name. Each counter contains one property per selected label.\n"
			" *\n"
			" * Example output:\n"
			" *   {\n"
			" *     \"Left\":  { \"human\": 2219, \"car\": 61223, \"bike\": 1425, \"bus\": 1576, \"truck\": 9646, \"other\": 142 },\n"
			" *     \"Right\": { \"human\": 2820, \"car\": 64687, \"bike\": 1004, \"bus\": 676,  \"truck\": 7118, \"other\": 123 }\n"
			" *   }\n"
			" */\n"
			"function decodeCounting(bytes) {\n"
			"  var result = {};\n"
			"  var offset = 0;\n\n"
			"  for (var i = 0; i < counterScenarios.length; i++) {\n"
			"    var counter = counterScenarios[i];\n"
			"    var counterKey = safeKey(counter.name);\n"
			"    result[counterKey] = {};\n"
			"    for (var c = 0; c < counter.classes.length; c++) {\n"
			"      if (offset + 2 > bytes.length) {\n"
			"        result[counterKey].error = 'Payload ended before all counting values were read';\n"
			"        return result;\n"
			"      }\n"
			"      var classKey = safeKey(counter.classes[c]);\n"
			"      result[counterKey][classKey] = readU16(bytes, offset);\n"
			"      offset += 2;\n"
			"    }\n"
			"  }\n\n"
			"  return result;\n"
			"}\n\n");

	pos += snprintf(js + pos, sizeof(js) - pos,
			"/**\n"
			" * Occupancy decoder\n"
			" *\n"
			" * Input:\n"
			" *   bytes - LoRaWAN payload bytes from the Occupancy port.\n"
			" *\n"
			" * Buffer data structure:\n"
			" *   The payload is repeated per configured occupancy area in the order shown in occupancyScenarios below.\n"
			" *   Each area block starts with a two-byte header followed by one uint8 value per selected label:\n"
			" *     byte 0: labelCount, the number of following label values for this area.\n"
			" *     byte 1: valueType, where 0=max, 1=min, 2=avg.\n"
			" *     bytes 2..N: labelCount uint8 values in the selected label order.\n"
			" *   Labels use this fixed order when enabled: human, car, bike, bus, truck, other.\n"
			" *   Values are unscaled uint8 OccupancyInArea EventInterval values, rounded and clamped to 0..255.\n"
			" *\n"
			" * Output:\n"
			" *   An object keyed by sanitized area name. Each area contains type plus one property per selected label.\n"
			" *\n"
			" * Example output:\n"
			" *   {\n"
			" *     \"Area_1\": { \"type\": \"max\", \"human\": 0 },\n"
			" *     \"Area_2\": { \"type\": \"max\", \"car\": 1 }\n"
			" *   }\n"
			" */\n"
			"function decodeOccupancy(bytes) {\n"
			"  var result = {};\n"
			"  var offset = 0;\n\n"
			"  for (var o = 0; o < occupancyScenarios.length; o++) {\n"
			"    var occupancy = occupancyScenarios[o];\n"
			"    var areaKey = safeKey(occupancy.name);\n"
			"    result[areaKey] = {};\n"
			"    if (offset + 2 > bytes.length) {\n"
			"      result[areaKey].error = 'Missing occupancy header';\n"
			"      result[areaKey].expectedType = normalizeType(occupancy.value);\n"
			"      break;\n"
			"    }\n"
			"    var labelCount = bytes[offset++];\n"
			"    var valueTypeCode = bytes[offset++];\n"
			"    var valueType = occupancyValueTypes[valueTypeCode] || ('unknown_' + valueTypeCode);\n"
			"    result[areaKey].type = valueType;\n"
			"    for (var oc = 0; oc < labelCount; oc++) {\n"
			"      if (offset >= bytes.length) {\n"
			"        result[areaKey].error = 'Payload ended before all occupancy values were read';\n"
			"        break;\n"
			"      }\n"
			"      var occupancyClass = safeKey(occupancy.classes[oc] || ('unknown_' + oc));\n"
			"      result[areaKey][occupancyClass] = bytes[offset++];\n"
			"    }\n"
			"    var expectedType = normalizeType(occupancy.value);\n"
			"    if (valueType !== expectedType) result[areaKey].expectedType = expectedType;\n"
			"    if (labelCount !== occupancy.classes.length) result[areaKey].expectedLabelCount = occupancy.classes.length;\n"
			"  }\n\n"
			"  return result;\n"
			"}\n\n");

	pos += snprintf(js + pos, sizeof(js) - pos, "function decodeByPort(port, bytes) {\n");
	pos += snprintf(js + pos, sizeof(js) - pos, "  if (port === countingPort) return decodeCounting(bytes);\n");
	pos += snprintf(js + pos, sizeof(js) - pos, "  if (port === occupancyPort) return decodeOccupancy(bytes);\n");
	pos += snprintf(js + pos, sizeof(js) - pos,
		"  return { error: 'Unsupported port ' + port, port: port };\n"
		"}\n");
	free(js_counter_defs);
	free(js_occupancy_defs);
	cJSON_Delete(counter_defs);
	cJSON_Delete(occupancy_defs);
	
	pthread_mutex_unlock(&g_occupancy_mutex);
	pthread_mutex_unlock(&g_counter_mutex);
	pthread_mutex_unlock(&g_publish_mutex);
	
	// Build filename: aib100-decoder-{model}-{serial}-{date}.js
	char filename[128];
	snprintf(filename, sizeof(filename), "aib100-decoder-%s-%s-%s.js",
	         dev_model, dev_serial, dev_date);

	// Send as downloadable JavaScript file
	ACAP_HTTP_Header_FILE(response, filename, "application/javascript", strlen(js));
	ACAP_HTTP_Respond_String(response, "%s", js);
}

// ==================================================================
// Signal Handler
// ==================================================================

static gboolean
signal_handler(gpointer user_data) {
    LOG("Received SIGTERM, initiating shutdown\n");
    running = 0;
    
    if (main_loop && g_main_loop_is_running(main_loop)) {
        g_main_loop_quit(main_loop);
    }
    return G_SOURCE_REMOVE;
}

// ==================================================================
// Main
// ==================================================================

int main(void) {
    openlog(APP_PACKAGE, LOG_PID|LOG_CONS, LOG_USER);
    LOG("========================================\n");
    LOG("Starting %s\n", APP_PACKAGE);
    LOG("========================================\n");

    g_app_start_time = time(NULL);

    // Initialize ACAP
	cJSON* initial_settings = ACAP_Init(APP_PACKAGE, Settings_Updated_Callback);
	Migrate_Settings(initial_settings);
    
    // Load saved counters
    LOG("Loading saved counters...\n");
    Load_Counters_From_File();
    
    // Setup AOA event subscriptions
    LOG("Setting up AOA event subscriptions...\n");
    ACAP_EVENTS_SetCallback(AOA_Event_Callback);
    eventSubscriptions = ACAP_FILE_Read("settings/subscriptions.json");
    if (eventSubscriptions) {
        cJSON* subscription = eventSubscriptions->child;
        int count = 0;
        while (subscription) {
            ACAP_EVENTS_Subscribe(subscription, NULL);
            count++;
            subscription = subscription->next;
        }
        LOG("Subscribed to %d event topic(s)\n", count);
    } else {
        LOG_WARN("No event subscriptions found\n");
    }
    
    // Load settings
    cJSON* settings = ACAP_Get_Config("settings");
    if (settings) {
        cJSON* b100 = cJSON_GetObjectItem(settings, "b100");
        if (b100) {
            cJSON* ip = cJSON_GetObjectItem(b100, "ip");
            if (ip && ip->valuestring) {
                strncpy(g_b100_ip, ip->valuestring, sizeof(g_b100_ip) - 1);
            }
            cJSON* cbip = cJSON_GetObjectItem(b100, "callbackIP");
            if (cbip && cbip->valuestring && cbip->valuestring[0]) {
                strncpy(g_callback_ip, cbip->valuestring, sizeof(g_callback_ip) - 1);
            }
            cJSON* cbport = cJSON_GetObjectItem(b100, "callbackPort");
            if (cbport && cbport->valueint > 0)
                g_callback_port = cbport->valueint;
			cJSON* cbuser = cJSON_GetObjectItem(b100, "callbackDigestUser");
			if (cbuser && cbuser->valuestring) {
				strncpy(g_callback_digest_user, cbuser->valuestring, sizeof(g_callback_digest_user) - 1);
				g_callback_digest_user[sizeof(g_callback_digest_user) - 1] = '\0';
			}
			cJSON* cbpass = cJSON_GetObjectItem(b100, "callbackDigestPassword");
			if (cbpass && cbpass->valuestring) {
				strncpy(g_callback_digest_password, cbpass->valuestring, sizeof(g_callback_digest_password) - 1);
				g_callback_digest_password[sizeof(g_callback_digest_password) - 1] = '\0';
			}
            cJSON* port = cJSON_GetObjectItem(b100, "port");
            if (port) {
                g_b100_port = port->valueint;
            }
        }
        
        cJSON* lorawan = cJSON_GetObjectItem(settings, "lorawan");
        if (lorawan) {
            cJSON* confirmed = cJSON_GetObjectItem(lorawan, "confirmed");
            if (confirmed) {
                // confirmed mode is managed automatically; ignore the stored setting
            }
        }
        
        cJSON* polling = cJSON_GetObjectItem(settings, "polling");
        if (polling) {
            cJSON* health = cJSON_GetObjectItem(polling, "healthCheckIntervalSeconds");
            if (health) {
                g_health_check_interval = health->valueint;
            }
        }
        
        // Load transmission settings
        cJSON* transmission = cJSON_GetObjectItem(settings, "transmission");
        if (transmission) {
			cJSON* counting = cJSON_GetObjectItem(transmission, "counting");
			cJSON* occupancy = cJSON_GetObjectItem(transmission, "occupancy");
			if (counting || occupancy) {
				Load_Counting_Publish_Config(counting);
				Load_Occupancy_Publish_Config(occupancy);
			} else {
				Load_Counting_Publish_Config(transmission);
				cJSON* old_enabled = cJSON_GetObjectItem(transmission, "enabled");
				if (old_enabled && !cJSON_IsTrue(old_enabled)) g_counting_port = 0;
				cJSON* old_value = cJSON_GetObjectItem(transmission, "occupancyValue");
				if (old_value && old_value->valuestring && Is_Valid_Occupancy_Value(old_value->valuestring)) {
					strncpy(g_occupancy_value, old_value->valuestring, sizeof(g_occupancy_value) - 1);
					g_occupancy_value[sizeof(g_occupancy_value) - 1] = '\0';
				}
			}
        }
    }
    
    // Initialize B100 client
    LOG("Initializing B100 client: %s:%d\n", g_b100_ip, g_b100_port);
    B100_Init(g_b100_ip, g_b100_port, 30);
    B100_Set_Downlink_Callback(B100_Downlink_Handler);
    B100_Set_Status_Callback(B100_Status_Handler);
    B100_Set_GPS_Callback(B100_GPS_Handler);
    
    // Initial connection test using /info endpoint
    ACAP_STATUS_SetString("app", "status", "Testing B100 connection...");
    if (B100_Test_Connection()) {
        LOG("B100 connection successful\n");
        
        // Configure B100 API enable and callback URIs
        {
            const char* cam_ip = g_callback_ip[0] ? g_callback_ip : ACAP_DEVICE_Prop("IPv4");
            char status_uri[64], receive_uri[64];
            snprintf(status_uri, sizeof(status_uri), "/local/%s/b100_status", APP_PACKAGE);
            snprintf(receive_uri, sizeof(receive_uri), "/local/%s/b100_receive", APP_PACKAGE);
			if (B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri,
										 g_callback_digest_user, g_callback_digest_password)) {
                g_callbacks_configured = 1;
                // Configure GPS callback — POST every 60 s to the b100_gps endpoint
                char gps_uri[64];
                snprintf(gps_uri, sizeof(gps_uri), "/local/%s/b100_gps", APP_PACKAGE);
                B100_Configure_GPS_Callback(gps_uri, 60);
                ACAP_STATUS_SetString("app", "status", "Running");
            } else {
                LOG_WARN("Failed to configure B100 callbacks\n");
                ACAP_STATUS_SetString("app", "status", "Running (callbacks failed)");
            }
        }

        // Trigger initial status request
        if (g_callbacks_configured)
            B100_Request_Status();
    } else {
        LOG_WARN("B100 connection failed - will retry in background\n");
        ACAP_STATUS_SetString("app", "status", "B100 connection failed - retrying...");
    }
    
    // Register HTTP endpoints
    ACAP_HTTP_Node("join", HTTP_Endpoint_join);
    ACAP_HTTP_Node("test", HTTP_Endpoint_test);
    ACAP_HTTP_Node("restart", HTTP_Endpoint_restart);
    ACAP_HTTP_Node("send", HTTP_Endpoint_send);
    ACAP_HTTP_Node("counters", HTTP_Endpoint_counters);
    ACAP_HTTP_Node("publish", HTTP_Endpoint_publish);
    ACAP_HTTP_Node("translator", HTTP_Endpoint_translator);
    ACAP_HTTP_Node("delete_counter", HTTP_Endpoint_delete_counter);
    ACAP_HTTP_Node("sync_counters", HTTP_Endpoint_sync_counters);
    ACAP_HTTP_Node("set_counters", HTTP_Endpoint_set_counters);
    ACAP_HTTP_Node("receive", HTTP_Endpoint_receive);
    ACAP_HTTP_Node("b100_status", HTTP_Endpoint_B100_Status_Callback);
    ACAP_HTTP_Node("b100_receive", HTTP_Endpoint_B100_Receive_Callback);
    ACAP_HTTP_Node("b100_gps", HTTP_Endpoint_B100_GPS_Callback);
    ACAP_HTTP_Node("gps", HTTP_Endpoint_gps);
    ACAP_HTTP_Node("b100_info", HTTP_Endpoint_b100_info);
    ACAP_HTTP_Node("b100_params", HTTP_Endpoint_b100_params);
    ACAP_HTTP_Node("b100_request_status", HTTP_Endpoint_b100_request_status);
    ACAP_HTTP_Node("linkcheck", HTTP_Endpoint_linkcheck);
    
	// Initialize next publish times
    pthread_mutex_lock(&g_publish_mutex);
	g_next_counting_publish_time = time(NULL) + (g_counting_interval_minutes * 60);
	g_next_occupancy_publish_time = time(NULL) + (g_occupancy_interval_minutes * 60);
    pthread_mutex_unlock(&g_publish_mutex);
    
    // Start background threads
    LOG("Starting background threads...\n");
    pthread_create(&health_thread, NULL, Health_Monitor_Thread, NULL);
    pthread_create(&publish_thread, NULL, Publish_Thread, NULL);
    
    // Setup signal handler
    LOG("Entering main loop\n");
	main_loop = g_main_loop_new(NULL, FALSE);
    GSource *signal_source = g_unix_signal_source_new(SIGTERM);
    if (signal_source) {
		g_source_set_callback(signal_source, signal_handler, NULL, NULL);
		g_source_attach(signal_source, NULL);
	} else {
		LOG_WARN("Signal detection failed\n");
	}
	
    g_main_loop_run(main_loop);
	
	// Cleanup
	LOG("Shutting down...\n");
	running = 0;
	
	pthread_join(health_thread, NULL);
	pthread_join(publish_thread, NULL);
	
	// Save counters before exit
	LOG("Saving counters before shutdown...\n");
	Save_Counters_To_File();
	
	// Cleanup event subscriptions
	if (eventSubscriptions) {
		cJSON_Delete(eventSubscriptions);
		eventSubscriptions = NULL;
	}
	
	B100_Cleanup();
    ACAP_Cleanup();
    
    LOG("Shutdown complete\n");
    closelog();
    return 0;
}
