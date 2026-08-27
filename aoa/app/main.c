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
#include "counter.h"
#include "occupancy.h"
#include "presence.h"
#include "ota_protocol.h"
#include "ota_scene.h"
#include "ota_translator.h"

#define APP_PACKAGE	"aib100"
#define DOWNLINK_LOG_MAX 10
#define PUBLISH_LOG_MAX 10
#define MAX_AOA_ITEMS 10
#define SETTINGS_VERSION 9
#define LORA_PORT_COUNTING 1
#define LORA_PORT_OCCUPANCY 2
#define LORA_PORT_PRESENCE 3
#define LORA_PORT_DOWNLINK_CONTROL 100
#define LORA_PORT_DOWNLINK_CONFIG 110
#define LORA_PORT_DOWNLINK_QUERY 120
#define LORA_PORT_USE_CASE_CONFIG 130
#define LORA_PORT_COUNTING_OTA 131
#define LORA_PORT_OCCUPANCY_OTA 132
#define LORA_PORT_PRESENCE_OTA 133
#define OTA_RESPONSE_QUEUE_MAX 32
#define OTA_INFORMATION_FORMAT_STRUCTURED 0x81

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
static pthread_mutex_t g_presence_publish_mutex = PTHREAD_MUTEX_INITIALIZER;
static GQueue* g_ota_response_queue = NULL;
static GQueue* g_ota_bridge_config_queue = NULL;
static pthread_mutex_t g_ota_response_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	int port;
	size_t length;
	uint8_t data[OTA_MAX_FRAME_SIZE];
	time_t next_attempt;
	int restart_bridge_after_send;
} OTA_Queued_Response;

typedef struct {
	uint8_t transaction_id;
	uint8_t field_mask;
	uint8_t data_rate;
	uint8_t adr_enabled;
} OTA_Queued_Bridge_Config;

static void Record_Publish_Log(int port, const unsigned char* data, size_t length);

// Settings cache
static char g_b100_ip[64] = "192.168.1.250";
static int g_b100_port = 81;
static char g_b100_api_user[33] = "lorabridge";
static char g_b100_api_password[65] = "lorabridge";
static char g_callback_ip[64] = "192.168.1.200";
static int g_callback_port = 80;
static char g_callback_digest_user[32] = "lorabridge";
static char g_callback_digest_password[32] = "lorabridge";
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

static int Clamp_Publish_Interval(int minutes) {
	if (minutes < 1) return 1;
	if (minutes > 60) return 60;
	return minutes;
}

static int Clamp_Publish_Port(int port) {
	if (port < 0) return 0;
	if (port > 223) return 223;
	return port;
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

static void Set_Bool(cJSON* parent, const char* key, int value) {
	cJSON* item = cJSON_GetObjectItem(parent, key);
	if (item) cJSON_ReplaceItemInObject(parent, key, cJSON_CreateBool(value ? 1 : 0));
	else cJSON_AddBoolToObject(parent, key, value ? 1 : 0);
}

static void Set_String(cJSON* parent, const char* key, const char* value) {
	cJSON* item = cJSON_GetObjectItem(parent, key);
	if (item) cJSON_ReplaceItemInObject(parent, key, cJSON_CreateString(value));
	else cJSON_AddStringToObject(parent, key, value);
}

static int Initialize_Counting_References(void) {
	const char* endpoint = "/local/objectanalytics/control.cgi";
	const char* config_request =
		"{\"apiVersion\":\"1.6\",\"context\":\"aib100\",\"method\":\"getConfiguration\"}";
	char* config_response = ACAP_VAPIX_Post(endpoint, config_request);
	if (!config_response) {
		LOG_WARN("Unable to read AOA configuration for counting initialization\n");
		return 0;
	}

	cJSON* config_root = cJSON_Parse(config_response);
	free(config_response);
	cJSON* data = config_root ? cJSON_GetObjectItem(config_root, "data") : NULL;
	cJSON* scenarios = data ? cJSON_GetObjectItem(data, "scenarios") : NULL;
	if (!scenarios || !cJSON_IsArray(scenarios)) {
		LOG_WARN("AOA configuration did not contain scenarios\n");
		cJSON_Delete(config_root);
		return 0;
	}

	int initialized = 0;
	cJSON* scenario = scenarios->child;
	while (scenario) {
		cJSON* type = cJSON_GetObjectItem(scenario, "type");
		cJSON* id = cJSON_GetObjectItem(scenario, "id");
		cJSON* name = cJSON_GetObjectItem(scenario, "name");
		if (type && cJSON_IsString(type) && strcmp(type->valuestring, "crosslinecounting") == 0 &&
		    id && cJSON_IsNumber(id) && name && cJSON_IsString(name) &&
		    Counting_Has_Scenario(name->valuestring)) {
			cJSON* request = cJSON_CreateObject();
			cJSON_AddStringToObject(request, "apiVersion", "1.6");
			cJSON_AddStringToObject(request, "context", "aib100");
			cJSON_AddStringToObject(request, "method", "getAccumulatedCounts");
			cJSON* params = cJSON_CreateObject();
			cJSON_AddNumberToObject(params, "scenario", id->valueint);
			cJSON_AddItemToObject(request, "params", params);
			char* request_text = cJSON_PrintUnformatted(request);
			cJSON_Delete(request);

			char* response_text = request_text ? ACAP_VAPIX_Post(endpoint, request_text) : NULL;
			free(request_text);
			cJSON* response = response_text ? cJSON_Parse(response_text) : NULL;
			free(response_text);
			cJSON* counts = response ? cJSON_GetObjectItem(response, "data") : NULL;
			if (counts && cJSON_IsObject(counts)) {
				cJSON_AddStringToObject(counts, "scenarioType", "CrosslineCounting");
				cJSON_AddStringToObject(counts, "scenario", name->valuestring);
				cJSON_AddStringToObject(counts, "reason", "startup snapshot");
				Counting_Process_AOA_Event(counts);
				initialized++;
			} else {
				LOG_WARN("Unable to read accumulated counts for AOA scenario %s\n", name->valuestring);
			}
			cJSON_Delete(response);
		}
		scenario = scenario->next;
	}
	cJSON_Delete(config_root);
	LOG("Initialized %d counting scenario reference(s) from AOA\n", initialized);
	return initialized;
}

static void Set_Object_Duplicate(cJSON* parent, const char* key, cJSON* source) {
	if (!source) return;
	cJSON* copy = cJSON_Duplicate(source, 1);
	if (!copy) return;
	cJSON* item = cJSON_GetObjectItem(parent, key);
	if (item) cJSON_ReplaceItemInObject(parent, key, copy);
	else cJSON_AddItemToObject(parent, key, copy);
}

static void Reset_Publish_Schedule(void) {
	time_t now = time(NULL);
	Counting_Reset_Schedule(now);
	Occupancy_Reset_Schedule(now);
}

static int Downlink_Command_Enabled_Value(cJSON* command_list, int port_a, int port_b, int port_c, int byte, int default_enabled) {
	if (!command_list || !cJSON_IsArray(command_list)) return default_enabled;
	cJSON* cmd = NULL;
	cJSON_ArrayForEach(cmd, command_list) {
		cJSON* p = cJSON_GetObjectItem(cmd, "port");
		cJSON* b = cJSON_GetObjectItem(cmd, "byte");
		if (!p || !b || b->valueint != byte) continue;
		if (p->valueint == port_a || p->valueint == port_b || p->valueint == port_c) {
			cJSON* e = cJSON_GetObjectItem(cmd, "enabled");
			return (!e || cJSON_IsTrue(e)) ? 1 : 0;
		}
	}
	return default_enabled;
}

static void Add_Downlink_Command(cJSON* command_list, int port, int byte, int enabled) {
	cJSON* cmd = cJSON_CreateObject();
	if (!cmd) return;
	cJSON_AddNumberToObject(cmd, "port", port);
	cJSON_AddNumberToObject(cmd, "byte", byte);
	cJSON_AddBoolToObject(cmd, "enabled", enabled ? 1 : 0);
	cJSON_AddItemToArray(command_list, cmd);
}

static void Set_Downlink_Command_Defaults(cJSON* settings) {
	cJSON* existing = cJSON_GetObjectItem(settings, "downlinkCommands");
	cJSON* replacement = cJSON_CreateArray();
	if (!replacement) return;

	const int ports[] = {
		LORA_PORT_DOWNLINK_CONTROL, LORA_PORT_DOWNLINK_CONFIG, LORA_PORT_DOWNLINK_QUERY,
		LORA_PORT_USE_CASE_CONFIG, LORA_PORT_COUNTING_OTA,
		LORA_PORT_OCCUPANCY_OTA, LORA_PORT_PRESENCE_OTA
	};
	const uint8_t command_masks[] = {
		0x06, 0x07, 0x05, 0x07, 0x0F, 0x0F, 0x0F
	};
	for (size_t index = 0; index < sizeof(ports) / sizeof(ports[0]); index++) {
		for (int command = OTA_COMMAND_GET; command <= OTA_COMMAND_LIST; command++) {
			if (!(command_masks[index] & (1u << (command - 1)))) continue;
			Add_Downlink_Command(replacement, ports[index], command,
				Downlink_Command_Enabled_Value(existing, ports[index], ports[index],
					ports[index], command, 1));
		}
	}

	if (existing) cJSON_ReplaceItemInObject(settings, "downlinkCommands", replacement);
	else cJSON_AddItemToObject(settings, "downlinkCommands", replacement);
}

static int Stream_Enabled_From_Config(cJSON* stream, int default_enabled) {
	if (!stream) return default_enabled;
	cJSON* enabled = cJSON_GetObjectItem(stream, "enabled");
	if (enabled) return cJSON_IsTrue(enabled);
	cJSON* port = cJSON_GetObjectItem(stream, "port");
	if (port) return Clamp_Publish_Port(port->valueint) > 0;
	return default_enabled;
}

static void Normalize_Transmission_V4(cJSON* settings) {
	cJSON* transmission = Ensure_Object(settings, "transmission");
	cJSON* counting = Ensure_Object(transmission, "counting");
	cJSON* occupancy = Ensure_Object(transmission, "occupancy");
	cJSON* presence = Ensure_Object(transmission, "presence");

	Set_Bool(counting, "enabled", Stream_Enabled_From_Config(counting, 1));
	Set_Number(counting, "port", LORA_PORT_COUNTING);
	Ensure_Object(counting, "scenarios");

	Set_Bool(occupancy, "enabled", Stream_Enabled_From_Config(occupancy, 0));
	Set_Number(occupancy, "port", LORA_PORT_OCCUPANCY);
	Ensure_Object(occupancy, "scenarios");

	Set_Bool(presence, "enabled", Stream_Enabled_From_Config(presence, 0));
	Set_Number(presence, "port", LORA_PORT_PRESENCE);
	cJSON_DeleteItemFromObject(presence, "heartbeatMinutes");
	cJSON_DeleteItemFromObject(presence, "triggerSeconds");
	int default_cooldown = 30;
	cJSON* old_cooldown = cJSON_GetObjectItem(presence, "cooldownSeconds");
	if (old_cooldown && cJSON_IsNumber(old_cooldown)) default_cooldown = old_cooldown->valueint;
	cJSON* old_schedule = cJSON_GetObjectItem(presence, "schedule");
	cJSON* scenarios = Ensure_Object(presence, "scenarios");
	for (cJSON* scenario = scenarios->child; scenario; scenario = scenario->next) {
		if (!cJSON_IsObject(scenario)) continue;
		if (!cJSON_GetObjectItem(scenario, "cooldownSeconds")) Set_Number(scenario, "cooldownSeconds", default_cooldown);
		cJSON* schedule = Ensure_Object(scenario, "schedule");
		cJSON* old_enabled = old_schedule ? cJSON_GetObjectItem(old_schedule, "enabled") : NULL;
		cJSON* old_start = old_schedule ? cJSON_GetObjectItem(old_schedule, "start") : NULL;
		cJSON* old_end = old_schedule ? cJSON_GetObjectItem(old_schedule, "end") : NULL;
		if (!cJSON_GetObjectItem(schedule, "enabled")) Set_Bool(schedule, "enabled", old_enabled && cJSON_IsTrue(old_enabled));
		if (!cJSON_GetObjectItem(schedule, "start")) Set_String(schedule, "start", old_start && cJSON_IsString(old_start) ? old_start->valuestring : "18:00");
		if (!cJSON_GetObjectItem(schedule, "end")) Set_String(schedule, "end", old_end && cJSON_IsString(old_end) ? old_end->valuestring : "06:00");
		cJSON_DeleteItemFromObject(scenario, "enabled");
	}
	cJSON_DeleteItemFromObject(presence, "cooldownSeconds");
	cJSON_DeleteItemFromObject(presence, "schedule");

	Set_Downlink_Command_Defaults(settings);
}

static void Normalize_B100_V5(cJSON* settings) {
	cJSON* b100 = Ensure_Object(settings, "b100");
	if (!cJSON_GetObjectItem(b100, "port")) Set_Number(b100, "port", 81);
	if (!cJSON_GetObjectItem(b100, "apiDigestUser")) Set_String(b100, "apiDigestUser", "lorabridge");
	if (!cJSON_GetObjectItem(b100, "apiDigestPassword")) Set_String(b100, "apiDigestPassword", "lorabridge");
}

static void Migrate_Settings(cJSON* settings) {
	if (!settings) return;

	cJSON* saved = ACAP_FILE_Read("localdata/settings.json");
	cJSON* saved_version_json = saved ? cJSON_GetObjectItem(saved, "settingsVersion") : NULL;
	int saved_version = saved_version_json ? saved_version_json->valueint : (saved ? 1 : SETTINGS_VERSION);

	if (saved_version >= SETTINGS_VERSION) {
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 8) {
		LOG("Migrating settings from version 8 to %d with framed OTA configuration\n", SETTINGS_VERSION);
		cJSON_DeleteItemFromObject(settings, "downlinkCommands");
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 7) {
		LOG("Migrating settings from version 7 to %d with per-alert Presence settings\n", SETTINGS_VERSION);
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 6) {
		LOG("Migrating settings from version 6 to %d with independent Presence Alert scenes\n", SETTINGS_VERSION);
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		cJSON* transmission = Ensure_Object(settings, "transmission");
		cJSON* presence = Ensure_Object(transmission, "presence");
		cJSON_ReplaceItemInObject(presence, "scenarios", cJSON_CreateObject());
		Set_Bool(presence, "enabled", 0);
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 5) {
		LOG("Migrating settings from version 5 to %d with Presence alerts\n", SETTINGS_VERSION);
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 4) {
		LOG("Migrating settings from version 4 to %d with bridge API authentication\n", SETTINGS_VERSION);
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 3) {
		LOG("Migrating settings from version 3 to %d with fixed LoRaWAN ports\n", SETTINGS_VERSION);
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	if (saved_version == 2) {
		LOG("Migrating settings from version 2 to %d\n", SETTINGS_VERSION);
		Normalize_B100_V5(settings);
		Normalize_Transmission_V4(settings);
		Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
		ACAP_FILE_Write("localdata/settings.json", settings);
		if (saved) cJSON_Delete(saved);
		return;
	}

	LOG("Migrating settings from version %d to %d\n", saved_version, SETTINGS_VERSION);

	cJSON* transmission = Ensure_Object(settings, "transmission");
	cJSON* counting = Ensure_Object(transmission, "counting");
	cJSON* occupancy = Ensure_Object(transmission, "occupancy");
	cJSON* presence = Ensure_Object(transmission, "presence");
	cJSON* old_transmission = saved ? cJSON_GetObjectItem(saved, "transmission") : NULL;
	cJSON* old_lorawan = saved ? cJSON_GetObjectItem(saved, "lorawan") : NULL;

	int interval = 15;
	int enabled = 1;
	int old_port = LORA_PORT_COUNTING;
	const char* occupancy_value = "average";
	cJSON* old_classes = NULL;

	if (old_transmission) {
		cJSON* interval_json = cJSON_GetObjectItem(old_transmission, "intervalMinutes");
		if (interval_json) interval = Clamp_Publish_Interval(interval_json->valueint);
		cJSON* enabled_json = cJSON_GetObjectItem(old_transmission, "enabled");
		if (enabled_json) enabled = cJSON_IsTrue(enabled_json);
		cJSON* old_value = cJSON_GetObjectItem(old_transmission, "occupancyValue");
		if (old_value && old_value->valuestring && Occupancy_Is_Valid_Value(old_value->valuestring)) occupancy_value = old_value->valuestring;
		old_classes = cJSON_GetObjectItem(old_transmission, "classes");
	}
	if (old_lorawan) {
		cJSON* port_json = cJSON_GetObjectItem(old_lorawan, "port");
		if (port_json) old_port = Clamp_Publish_Port(port_json->valueint);
	}

	Set_Number(counting, "intervalMinutes", interval);
	Set_Bool(counting, "enabled", enabled && old_port > 0);
	Set_Number(counting, "port", LORA_PORT_COUNTING);
	if (old_classes) Set_Object_Duplicate(counting, "classes", old_classes);
	Ensure_Object(counting, "scenarios");

	Set_Number(occupancy, "intervalMinutes", interval);
	Set_Bool(occupancy, "enabled", 0);
	Set_Number(occupancy, "port", LORA_PORT_OCCUPANCY);
	Set_String(occupancy, "value", occupancy_value);
	if (old_classes) Set_Object_Duplicate(occupancy, "classes", old_classes);
	Ensure_Object(occupancy, "scenarios");
	Set_Bool(presence, "enabled", 0);
	Set_Number(presence, "port", LORA_PORT_PRESENCE);
	Ensure_Object(presence, "scenarios");
	Set_Downlink_Command_Defaults(settings);
	Normalize_B100_V5(settings);

	Set_Number(settings, "settingsVersion", SETTINGS_VERSION);
	ACAP_FILE_Write("localdata/settings.json", settings);

	if (saved) cJSON_Delete(saved);
}

// ==================================================================
// Event Callback
// ==================================================================

void
AOA_Event_Callback(cJSON *event, void* userdata) {
	Presence_Process_AOA_Event(event);
	cJSON* scenarioType = cJSON_GetObjectItem(event, "scenarioType");
	if (!scenarioType || !scenarioType->valuestring) return;

	if (Occupancy_Is_Scenario_Type(scenarioType->valuestring)) {
		Occupancy_Process_AOA_Event(event);
		return;
	}

	if (strcmp(scenarioType->valuestring, "CrosslineCounting") == 0) {
		Counting_Process_AOA_Event(event);
	}
}

// ==================================================================
// Settings Callback
// ==================================================================

void
Settings_Updated_Callback( const char* service, cJSON* data) {
	LOG_TRACE("Settings_Updated_Callback [%s]\n", service);

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
		cJSON* apiuser = cJSON_GetObjectItem(data, "apiDigestUser");
		if (apiuser && apiuser->valuestring) {
			strncpy(g_b100_api_user, apiuser->valuestring, sizeof(g_b100_api_user) - 1);
			g_b100_api_user[sizeof(g_b100_api_user) - 1] = '\0';
		}
		cJSON* apipass = cJSON_GetObjectItem(data, "apiDigestPassword");
		if (apipass && apipass->valuestring) {
			strncpy(g_b100_api_password, apipass->valuestring, sizeof(g_b100_api_password) - 1);
			g_b100_api_password[sizeof(g_b100_api_password) - 1] = '\0';
		}
		if (!B100_Set_API_Credentials(g_b100_api_user, g_b100_api_password))
			LOG_WARN("Bridge API credentials must both be set or both be empty\n");
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
		int old_counting_enabled = Counting_Enabled();
		int old_counting_interval = Counting_Interval_Minutes();
		int old_occupancy_enabled = Occupancy_Enabled();
		int old_occupancy_interval = Occupancy_Interval_Minutes();
		cJSON* counting = cJSON_GetObjectItem(data, "counting");
		cJSON* occupancy = cJSON_GetObjectItem(data, "occupancy");
		cJSON* presence = cJSON_GetObjectItem(data, "presence");
		if (counting || occupancy || presence) {
			Counting_Load_Config(counting, LORA_PORT_COUNTING);
			Occupancy_Load_Config(occupancy, LORA_PORT_OCCUPANCY);
			Presence_Load_Config(presence, LORA_PORT_PRESENCE);
			refresh_occupancy_status = occupancy != NULL;
		} else {
			Counting_Load_Config(data, LORA_PORT_COUNTING);
			cJSON* old_value = cJSON_GetObjectItem(data, "occupancyValue");
			if (old_value && old_value->valuestring && Occupancy_Is_Valid_Value(old_value->valuestring)) {
				cJSON* legacy_occupancy = cJSON_CreateObject();
				if (legacy_occupancy) {
					cJSON_AddStringToObject(legacy_occupancy, "value", old_value->valuestring);
					Occupancy_Load_Config(legacy_occupancy, LORA_PORT_OCCUPANCY);
					cJSON_Delete(legacy_occupancy);
				}
				refresh_occupancy_status = 1;
			}
		}
		time_t now = time(NULL);
		if (old_counting_enabled != Counting_Enabled() || old_counting_interval != Counting_Interval_Minutes())
			Counting_Reset_Schedule(now);
		if (old_occupancy_enabled != Occupancy_Enabled() || old_occupancy_interval != Occupancy_Interval_Minutes())
			Occupancy_Reset_Schedule(now);
		if (refresh_occupancy_status) Occupancy_Update_ACAP_Status();
		if (presence) {
			Presence_Initialize_State();
		}
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

static int Hex_Nibble(char value) {
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

static int Decode_Downlink_Bytes(const B100_Downlink* downlink, uint8_t* bytes, size_t capacity) {
	if (!downlink || !bytes || capacity == 0) return -1;
	if (strcmp(downlink->payload_type, "HEX") != 0) {
		size_t length = strlen(downlink->payload);
		if (length > capacity) return -1;
		memcpy(bytes, downlink->payload, length);
		return (int)length;
	}

	size_t hex_length = strlen(downlink->payload);
	if ((hex_length % 2) != 0 || hex_length / 2 > capacity) return -1;
	for (size_t index = 0; index < hex_length; index += 2) {
		int high = Hex_Nibble(downlink->payload[index]);
		int low = Hex_Nibble(downlink->payload[index + 1]);
		if (high < 0 || low < 0) return -1;
		bytes[index / 2] = (uint8_t)((high << 4) | low);
	}
	return (int)(hex_length / 2);
}

static int Queue_OTA_Frame(int port, uint8_t command, uint8_t transaction_id,
	const uint8_t* body, size_t body_length, int restart_bridge_after_send) {
	OTA_Queued_Response* queued = calloc(1, sizeof(*queued));
	if (!queued) {
		LOG_WARN("OTA: Failed to allocate response on port %d\n", port);
		return 0;
	}
	queued->port = port;
	queued->restart_bridge_after_send = restart_bridge_after_send;
	OTA_Status status = OTA_Encode_Frame(command, transaction_id, body, body_length,
		queued->data, sizeof(queued->data), &queued->length);
	if (status != OTA_STATUS_OK) {
		LOG_WARN("OTA: Failed to encode response on port %d: status=%d\n", port, status);
		free(queued);
		return 0;
	}

	pthread_mutex_lock(&g_ota_response_mutex);
	if (!g_ota_response_queue) g_ota_response_queue = g_queue_new();
	if (!g_ota_response_queue || g_queue_get_length(g_ota_response_queue) >= OTA_RESPONSE_QUEUE_MAX) {
		pthread_mutex_unlock(&g_ota_response_mutex);
		LOG_WARN("OTA: Response queue full, dropping response on port %d\n", port);
		free(queued);
		return 0;
	}
	g_queue_push_tail(g_ota_response_queue, queued);
	unsigned int queue_length = g_queue_get_length(g_ota_response_queue);
	pthread_mutex_unlock(&g_ota_response_mutex);
	ACAP_STATUS_SetString("lorawan", "lastOTAResponseResult", "queued");
	ACAP_STATUS_SetNumber("lorawan", "otaResponseQueueLength", (int)queue_length);
	LOG("OTA: Queued response on port %d (%zu bytes)\n", port, queued->length);
	return 1;
}

static int Send_OTA_Frame(int port, uint8_t command, uint8_t transaction_id,
	const uint8_t* body, size_t body_length) {
	return Queue_OTA_Frame(port, command, transaction_id, body, body_length, 0);
}

static void Drain_OTA_Response_Queue(void) {
	time_t now = time(NULL);
	pthread_mutex_lock(&g_ota_response_mutex);
	OTA_Queued_Response* queued = g_ota_response_queue ? g_queue_peek_head(g_ota_response_queue) : NULL;
	if (!queued || queued->next_attempt > now) {
		pthread_mutex_unlock(&g_ota_response_mutex);
		return;
	}
	queued = g_queue_pop_head(g_ota_response_queue);
	pthread_mutex_unlock(&g_ota_response_mutex);

	if (B100_Send_Bytes(queued->data, (int)queued->length, queued->port, 0)) {
		LOG("OTA: Response accepted on port %d (%zu bytes)\n", queued->port, queued->length);
		Record_Publish_Log(queued->port, queued->data, queued->length);
		ACAP_STATUS_SetString("lorawan", "lastOTAResponseResult", "accepted");
		int restart_bridge = queued->restart_bridge_after_send;
		free(queued);
		if (restart_bridge) {
			LOG("OTA: Restart Bridge acknowledgement accepted; restarting bridge\n");
			B100_Restart();
		}
	} else {
		queued->next_attempt = now + 5;
		ACAP_STATUS_SetString("lorawan", "lastOTAResponseResult", B100_Get_Last_Error());
		pthread_mutex_lock(&g_ota_response_mutex);
		g_queue_push_head(g_ota_response_queue, queued);
		pthread_mutex_unlock(&g_ota_response_mutex);
	}

	pthread_mutex_lock(&g_ota_response_mutex);
	unsigned int queue_length = g_ota_response_queue ? g_queue_get_length(g_ota_response_queue) : 0;
	pthread_mutex_unlock(&g_ota_response_mutex);
	ACAP_STATUS_SetNumber("lorawan", "otaResponseQueueLength", (int)queue_length);
}

static int Queue_OTA_Bridge_Config(const OTA_Frame* frame) {
	OTA_Queued_Bridge_Config* queued = calloc(1, sizeof(*queued));
	if (!queued) return 0;
	queued->transaction_id = frame->transaction_id;
	queued->field_mask = frame->body[0];
	queued->data_rate = frame->body[1];
	queued->adr_enabled = frame->body[2];

	pthread_mutex_lock(&g_ota_response_mutex);
	if (!g_ota_bridge_config_queue) g_ota_bridge_config_queue = g_queue_new();
	if (!g_ota_bridge_config_queue ||
		g_queue_get_length(g_ota_bridge_config_queue) >= OTA_RESPONSE_QUEUE_MAX) {
		pthread_mutex_unlock(&g_ota_response_mutex);
		free(queued);
		return 0;
	}
	g_queue_push_tail(g_ota_bridge_config_queue, queued);
	pthread_mutex_unlock(&g_ota_response_mutex);
	LOG("OTA: Queued bridge configuration update\n");
	return 1;
}

static void Drain_OTA_Bridge_Config_Queue(void) {
	pthread_mutex_lock(&g_ota_response_mutex);
	OTA_Queued_Bridge_Config* queued = g_ota_bridge_config_queue
		? g_queue_pop_head(g_ota_bridge_config_queue) : NULL;
	pthread_mutex_unlock(&g_ota_response_mutex);
	if (!queued) return;

	OTA_Status status = OTA_STATUS_OK;
	cJSON* params = cJSON_CreateObject();
	if (!params) status = OTA_STATUS_APPLY_FAILED;
	else {
		if (queued->field_mask & 0x01)
			cJSON_AddNumberToObject(params, "data_rate", queued->data_rate);
		if (queued->field_mask & 0x02)
			cJSON_AddNumberToObject(params, "adr_enable", queued->adr_enabled);
		if (!B100_Set_Params(params)) status = OTA_STATUS_APPLY_FAILED;
		cJSON_Delete(params);
	}
	uint8_t ack[] = {(uint8_t)status};
	Send_OTA_Frame(LORA_PORT_DOWNLINK_CONFIG, OTA_COMMAND_SET_ACK,
		queued->transaction_id, ack, sizeof(ack));
	free(queued);
}

static void Send_OTA_Error(int port, uint8_t transaction_id, uint8_t request_command, OTA_Status status) {
	uint8_t body[] = {request_command, (uint8_t)status};
	Send_OTA_Frame(port, OTA_COMMAND_ERROR, transaction_id, body, sizeof(body));
}

static cJSON* Transmission_Use_Case(cJSON* settings, uint8_t index) {
	cJSON* transmission = settings ? cJSON_GetObjectItem(settings, "transmission") : NULL;
	if (!transmission || !cJSON_IsObject(transmission)) return NULL;
	if (index == 1) return cJSON_GetObjectItem(transmission, "counting");
	if (index == 2) return cJSON_GetObjectItem(transmission, "occupancy");
	if (index == 3) return cJSON_GetObjectItem(transmission, "presence");
	return NULL;
}

static OTA_Status Build_Transmission_Config_Body(cJSON* settings, uint8_t index,
	uint8_t* body, size_t* body_length) {
	cJSON* use_case = Transmission_Use_Case(settings, index);
	if (!use_case || !body || !body_length) return OTA_STATUS_INVALID_VALUE;
	cJSON* enabled = cJSON_GetObjectItem(use_case, "enabled");
	body[0] = index;
	body[1] = enabled && cJSON_IsTrue(enabled) ? 1 : 0;
	if (index == 3) {
		*body_length = 2;
		return OTA_STATUS_OK;
	}
	cJSON* interval = cJSON_GetObjectItem(use_case, "intervalMinutes");
	int interval_minutes = interval && cJSON_IsNumber(interval) ? interval->valueint : 15;
	if (interval_minutes < 1 || interval_minutes > 60) return OTA_STATUS_INVALID_RANGE;
	body[2] = (uint8_t)interval_minutes;
	*body_length = 3;
	return OTA_STATUS_OK;
}

static OTA_Status Apply_Transmission_Config_Body(cJSON* settings, const uint8_t* body,
	size_t body_length) {
	if (!settings || !body || body_length < 2) return OTA_STATUS_INVALID_LENGTH;
	uint8_t index = body[0];
	if (index < 1 || index > 3) return OTA_STATUS_INVALID_VALUE;
	size_t expected_length = index == 3 ? 2 : 3;
	if (body_length != expected_length) return OTA_STATUS_INVALID_LENGTH;
	if (body[1] > 1) return OTA_STATUS_INVALID_VALUE;
	if (index != 3 && (body[2] < 1 || body[2] > 60)) return OTA_STATUS_INVALID_RANGE;

	cJSON* use_case = Transmission_Use_Case(settings, index);
	if (!use_case || !cJSON_IsObject(use_case)) return OTA_STATUS_APPLY_FAILED;
	cJSON* previous = cJSON_Duplicate(use_case, 1);
	if (!previous) return OTA_STATUS_APPLY_FAILED;
	Set_Bool(use_case, "enabled", body[1]);
	if (index != 3) Set_Number(use_case, "intervalMinutes", body[2]);
	if (!ACAP_FILE_Write("localdata/settings.json", settings)) {
		cJSON* transmission = cJSON_GetObjectItem(settings, "transmission");
		const char* key = index == 1 ? "counting" : index == 2 ? "occupancy" : "presence";
		cJSON_ReplaceItemInObject(transmission, key, previous);
		return OTA_STATUS_APPLY_FAILED;
	}
	cJSON_Delete(previous);

	cJSON* transmission = cJSON_GetObjectItem(settings, "transmission");
	Settings_Updated_Callback("transmission", transmission);
	return OTA_STATUS_OK;
}

static void Handle_Transmission_OTA(const OTA_Frame* frame, cJSON* settings) {
	uint8_t response_body[12];
	size_t response_length = 0;
	OTA_Status status;

	if (frame->command == OTA_COMMAND_GET) {
		if (frame->body_length != 1) {
			Send_OTA_Error(LORA_PORT_USE_CASE_CONFIG, frame->transaction_id,
				frame->command, OTA_STATUS_INVALID_LENGTH);
			return;
		}
		status = Build_Transmission_Config_Body(settings, frame->body[0],
			response_body, &response_length);
		if (status == OTA_STATUS_OK)
			Send_OTA_Frame(LORA_PORT_USE_CASE_CONFIG, OTA_COMMAND_GET_RESPONSE,
				frame->transaction_id, response_body, response_length);
		else
			Send_OTA_Error(LORA_PORT_USE_CASE_CONFIG, frame->transaction_id, frame->command, status);
		return;
	}

	if (frame->command == OTA_COMMAND_SET) {
		status = Apply_Transmission_Config_Body(settings, frame->body, frame->body_length);
		uint8_t index = frame->body_length > 0 ? frame->body[0] : 0;
		uint8_t ack[] = {index, (uint8_t)status};
		Send_OTA_Frame(LORA_PORT_USE_CASE_CONFIG, OTA_COMMAND_SET_ACK,
			frame->transaction_id, ack, sizeof(ack));
		return;
	}

	if (frame->command == OTA_COMMAND_CAPS) {
		if (frame->body_length != 0) {
			Send_OTA_Error(LORA_PORT_USE_CASE_CONFIG, frame->transaction_id,
				frame->command, OTA_STATUS_INVALID_LENGTH);
			return;
		}
		uint8_t caps[] = {1, 1, 1, 60, 2, 1, 1, 60, 3, 0, 0, 0};
		Send_OTA_Frame(LORA_PORT_USE_CASE_CONFIG, OTA_COMMAND_CAPS_RESPONSE,
			frame->transaction_id, caps, sizeof(caps));
		return;
	}

	Send_OTA_Error(LORA_PORT_USE_CASE_CONFIG, frame->transaction_id,
		frame->command, OTA_STATUS_UNKNOWN_COMMAND);
}

static void Handle_Action_OTA(const OTA_Frame* frame) {
	if (frame->command == OTA_COMMAND_CAPS) {
		if (frame->body_length != 0) {
			Send_OTA_Error(LORA_PORT_DOWNLINK_CONTROL, frame->transaction_id,
				frame->command, OTA_STATUS_INVALID_LENGTH);
			return;
		}
		uint8_t caps[] = {0x01, 0x02};
		Send_OTA_Frame(LORA_PORT_DOWNLINK_CONTROL, OTA_COMMAND_CAPS_RESPONSE,
			frame->transaction_id, caps, sizeof(caps));
		return;
	}

	if (frame->command != OTA_COMMAND_SET) {
		Send_OTA_Error(LORA_PORT_DOWNLINK_CONTROL, frame->transaction_id,
			frame->command, OTA_STATUS_UNKNOWN_COMMAND);
		return;
	}
	if (frame->body_length != 1) {
		Send_OTA_Error(LORA_PORT_DOWNLINK_CONTROL, frame->transaction_id,
			frame->command, OTA_STATUS_INVALID_LENGTH);
		return;
	}

	uint8_t action = frame->body[0];
	if (action == 0x01) {
		uint8_t ack[] = {action, OTA_STATUS_OK};
		Queue_OTA_Frame(LORA_PORT_DOWNLINK_CONTROL, OTA_COMMAND_SET_ACK,
			frame->transaction_id, ack, sizeof(ack), 1);
		return;
	}
	if (action == 0x02) {
		LOG("OTA: Reset all use-case data\n");
		Counting_Reset_All();
		int saved = Counting_Save_To_File();
		Occupancy_Reset_All();
		Presence_Reset_All();
		uint8_t ack[] = {action, saved ? OTA_STATUS_OK : OTA_STATUS_APPLY_FAILED};
		Send_OTA_Frame(LORA_PORT_DOWNLINK_CONTROL, OTA_COMMAND_SET_ACK,
			frame->transaction_id, ack, sizeof(ack));
		return;
	}

	uint8_t ack[] = {action, OTA_STATUS_INVALID_VALUE};
	Send_OTA_Frame(LORA_PORT_DOWNLINK_CONTROL, OTA_COMMAND_SET_ACK,
		frame->transaction_id, ack, sizeof(ack));
}

static void Handle_Bridge_Config_OTA(const OTA_Frame* frame) {
	if (frame->command == OTA_COMMAND_GET) {
		if (frame->body_length != 0) {
			Send_OTA_Error(LORA_PORT_DOWNLINK_CONFIG, frame->transaction_id,
				frame->command, OTA_STATUS_INVALID_LENGTH);
			return;
		}
		B100_Status* bridge = B100_Get_Status();
		if (!bridge->hasConfiguredDataRate || !bridge->hasConfiguredAdr) {
			Send_OTA_Error(LORA_PORT_DOWNLINK_CONFIG, frame->transaction_id,
				frame->command, OTA_STATUS_APPLY_FAILED);
			return;
		}
		uint8_t body[] = {0x03, (uint8_t)bridge->configuredDataRate,
			bridge->configuredAdr ? 1 : 0};
		Send_OTA_Frame(LORA_PORT_DOWNLINK_CONFIG, OTA_COMMAND_GET_RESPONSE,
			frame->transaction_id, body, sizeof(body));
		return;
	}

	if (frame->command == OTA_COMMAND_SET) {
		OTA_Status status = OTA_STATUS_OK;
		if (frame->body_length != 3) status = OTA_STATUS_INVALID_LENGTH;
		else if (frame->body[0] == 0 || (frame->body[0] & ~0x03) != 0 || frame->body[2] > 1)
			status = OTA_STATUS_INVALID_VALUE;
		else if ((frame->body[0] & 0x01) && frame->body[1] > 5)
			status = OTA_STATUS_INVALID_RANGE;
		else if (Queue_OTA_Bridge_Config(frame)) return;
		else status = OTA_STATUS_APPLY_FAILED;
		uint8_t ack[] = {(uint8_t)status};
		Send_OTA_Frame(LORA_PORT_DOWNLINK_CONFIG, OTA_COMMAND_SET_ACK,
			frame->transaction_id, ack, sizeof(ack));
		return;
	}

	if (frame->command == OTA_COMMAND_CAPS) {
		if (frame->body_length != 0) {
			Send_OTA_Error(LORA_PORT_DOWNLINK_CONFIG, frame->transaction_id,
				frame->command, OTA_STATUS_INVALID_LENGTH);
			return;
		}
		uint8_t caps[] = {0, 5, 1};
		Send_OTA_Frame(LORA_PORT_DOWNLINK_CONFIG, OTA_COMMAND_CAPS_RESPONSE,
			frame->transaction_id, caps, sizeof(caps));
		return;
	}

	Send_OTA_Error(LORA_PORT_DOWNLINK_CONFIG, frame->transaction_id,
		frame->command, OTA_STATUS_UNKNOWN_COMMAND);
}

static void Build_Camera_Info(char* output, size_t output_size) {
	const char* model = ACAP_DEVICE_Prop("model");
	const char* serial = ACAP_DEVICE_Prop("serial");
	const char* firmware = ACAP_DEVICE_Prop("firmware");
	const char* app_version = "?";
	cJSON* manifest = ACAP_Get_Config("manifest");
	cJSON* package = manifest ? cJSON_GetObjectItem(manifest, "acapPackageConf") : NULL;
	cJSON* setup = package ? cJSON_GetObjectItem(package, "setup") : NULL;
	cJSON* version = setup ? cJSON_GetObjectItem(setup, "version") : NULL;
	if (version && cJSON_IsString(version)) app_version = version->valuestring;
	snprintf(output, output_size, "%s,%s,%s,%dh,%s",
		model ? model : "?", serial ? serial : "?", firmware ? firmware : "?",
		(int)(ACAP_DEVICE_Uptime() / 3600.0), app_version);
}

static void Build_Bridge_Info(char* output, size_t output_size) {
	B100_Status* bridge = B100_Get_Status();
	snprintf(output, output_size, "%s/%s,%s,%s,%.0fC,R%u,%s",
		bridge->hardware[0] ? bridge->hardware : "?",
		bridge->hardwareVersion[0] ? bridge->hardwareVersion : "?",
		bridge->firmwareVersion[0] ? bridge->firmwareVersion : "?",
		bridge->powerSource[0] ? bridge->powerSource : "?", bridge->tempC,
		bridge->restartCounter, bridge->devAddrStr[0] ? bridge->devAddrStr : "0");
}

static int Append_Information_String(uint8_t* body, size_t* body_length,
	size_t capacity, size_t length_offset, const char* value) {
	const char* text = value && value[0] ? value : "?";
	size_t length = strlen(text);
	if (length > UINT8_MAX || *body_length + length > capacity) return 0;
	body[length_offset] = (uint8_t)length;
	memcpy(body + *body_length, text, length);
	*body_length += length;
	return 1;
}

static size_t Build_Structured_Camera_Info(uint8_t* body, size_t capacity) {
	if (!body || capacity < 10) return 0;
	const char* app_version = "?";
	cJSON* manifest = ACAP_Get_Config("manifest");
	cJSON* package = manifest ? cJSON_GetObjectItem(manifest, "acapPackageConf") : NULL;
	cJSON* setup = package ? cJSON_GetObjectItem(package, "setup") : NULL;
	cJSON* version = setup ? cJSON_GetObjectItem(setup, "version") : NULL;
	if (version && cJSON_IsString(version)) app_version = version->valuestring;

	uint32_t uptime_hours = (uint32_t)(ACAP_DEVICE_Uptime() / 3600.0);
	body[0] = 0x01;
	body[1] = OTA_INFORMATION_FORMAT_STRUCTURED;
	body[2] = body[3] = body[4] = body[5] = 0;
	body[6] = (uint8_t)(uptime_hours & 0xFF);
	body[7] = (uint8_t)((uptime_hours >> 8) & 0xFF);
	body[8] = (uint8_t)((uptime_hours >> 16) & 0xFF);
	body[9] = (uint8_t)((uptime_hours >> 24) & 0xFF);
	size_t body_length = 10;
	if (!Append_Information_String(body, &body_length, capacity, 2, ACAP_DEVICE_Prop("model")) ||
		!Append_Information_String(body, &body_length, capacity, 3, ACAP_DEVICE_Prop("serial")) ||
		!Append_Information_String(body, &body_length, capacity, 4, ACAP_DEVICE_Prop("firmware")) ||
		!Append_Information_String(body, &body_length, capacity, 5, app_version)) return 0;
	return body_length;
}

static size_t Build_Structured_Bridge_Info(uint8_t* body, size_t capacity) {
	if (!body || capacity < 11) return 0;
	B100_Status* bridge = B100_Get_Status();
	int temperature_tenths = (int)lround(bridge->tempC * 10.0);
	body[0] = 0x02;
	body[1] = OTA_INFORMATION_FORMAT_STRUCTURED;
	body[2] = body[3] = body[4] = body[5] = body[6] = 0;
	body[7] = (uint8_t)(temperature_tenths & 0xFF);
	body[8] = (uint8_t)((temperature_tenths >> 8) & 0xFF);
	body[9] = (uint8_t)(bridge->restartCounter & 0xFF);
	body[10] = (uint8_t)((bridge->restartCounter >> 8) & 0xFF);
	size_t body_length = 11;
	if (!Append_Information_String(body, &body_length, capacity, 2, bridge->hardware) ||
		!Append_Information_String(body, &body_length, capacity, 3, bridge->hardwareVersion) ||
		!Append_Information_String(body, &body_length, capacity, 4, bridge->firmwareVersion) ||
		!Append_Information_String(body, &body_length, capacity, 5, bridge->powerSource) ||
		!Append_Information_String(body, &body_length, capacity, 6, bridge->devAddrStr)) return 0;
	return body_length;
}

static void Handle_Information_OTA(const OTA_Frame* frame) {
	if (frame->command == OTA_COMMAND_CAPS) {
		if (frame->body_length != 0) {
			Send_OTA_Error(LORA_PORT_DOWNLINK_QUERY, frame->transaction_id,
				frame->command, OTA_STATUS_INVALID_LENGTH);
			return;
		}
		uint8_t caps[] = {0x01, 0x02, 43};
		Send_OTA_Frame(LORA_PORT_DOWNLINK_QUERY, OTA_COMMAND_CAPS_RESPONSE,
			frame->transaction_id, caps, sizeof(caps));
		return;
	}
	if (frame->command != OTA_COMMAND_GET) {
		Send_OTA_Error(LORA_PORT_DOWNLINK_QUERY, frame->transaction_id,
			frame->command, OTA_STATUS_UNKNOWN_COMMAND);
		return;
	}
	if (frame->body_length < 1 || frame->body_length > 2) {
		Send_OTA_Error(LORA_PORT_DOWNLINK_QUERY, frame->transaction_id,
			frame->command, OTA_STATUS_INVALID_LENGTH);
		return;
	}

	uint8_t info_type = frame->body[0];
	uint8_t page_index = frame->body_length == 2 ? frame->body[1] : 0;
	uint8_t structured_body[OTA_MAX_BODY_SIZE];
	size_t structured_length = info_type == 0x01
		? Build_Structured_Camera_Info(structured_body, sizeof(structured_body))
		: info_type == 0x02
			? Build_Structured_Bridge_Info(structured_body, sizeof(structured_body))
			: 0;
	if (structured_length > 0) {
		if (page_index != 0) {
			Send_OTA_Error(LORA_PORT_DOWNLINK_QUERY, frame->transaction_id,
				frame->command, OTA_STATUS_INVALID_RANGE);
			return;
		}
		Send_OTA_Frame(LORA_PORT_DOWNLINK_QUERY, OTA_COMMAND_GET_RESPONSE,
			frame->transaction_id, structured_body, structured_length);
		return;
	}

	char info[256] = {0};
	if (info_type == 0x01) Build_Camera_Info(info, sizeof(info));
	else if (info_type == 0x02) Build_Bridge_Info(info, sizeof(info));
	else {
		Send_OTA_Error(LORA_PORT_DOWNLINK_QUERY, frame->transaction_id,
			frame->command, OTA_STATUS_INVALID_VALUE);
		return;
	}

	const size_t max_chunk_length = 43;
	size_t info_length = strlen(info);
	size_t page_count = info_length == 0 ? 1 : (info_length + max_chunk_length - 1) / max_chunk_length;
	if (page_index >= page_count) {
		Send_OTA_Error(LORA_PORT_DOWNLINK_QUERY, frame->transaction_id,
			frame->command, OTA_STATUS_INVALID_RANGE);
		return;
	}
	size_t offset = page_index * max_chunk_length;
	size_t chunk_length = info_length - offset;
	if (chunk_length > max_chunk_length) chunk_length = max_chunk_length;
	uint8_t body[OTA_MAX_BODY_SIZE];
	body[0] = info_type;
	body[1] = page_index;
	body[2] = (uint8_t)page_count;
	body[3] = (uint8_t)chunk_length;
	memcpy(body + 4, info + offset, chunk_length);
	Send_OTA_Frame(LORA_PORT_DOWNLINK_QUERY, OTA_COMMAND_GET_RESPONSE,
		frame->transaction_id, body, chunk_length + 4);
}

static int Is_OTA_Port(int port) {
	return port == LORA_PORT_DOWNLINK_CONTROL || port == LORA_PORT_DOWNLINK_CONFIG ||
		port == LORA_PORT_DOWNLINK_QUERY || port == LORA_PORT_USE_CASE_CONFIG ||
		port == LORA_PORT_COUNTING_OTA || port == LORA_PORT_OCCUPANCY_OTA ||
		port == LORA_PORT_PRESENCE_OTA;
}

static void Dispatch_OTA_Frame(int port, const OTA_Frame* frame, cJSON* settings) {
	if (port == LORA_PORT_DOWNLINK_CONTROL) Handle_Action_OTA(frame);
	else if (port == LORA_PORT_DOWNLINK_CONFIG) Handle_Bridge_Config_OTA(frame);
	else if (port == LORA_PORT_DOWNLINK_QUERY) Handle_Information_OTA(frame);
	else if (port == LORA_PORT_USE_CASE_CONFIG) Handle_Transmission_OTA(frame, settings);
	else OTA_Scene_Handle(port, frame, settings, Send_OTA_Frame);
}

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
	uint8_t bytes[256] = {0};
	int byte_count = Decode_Downlink_Bytes(downlink, bytes, sizeof(bytes));
	if (byte_count <= 0) {
		LOG_WARN("Downlink: Invalid %s payload on port %d\n", downlink->payload_type, downlink->port);
		return;
	}

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
	if (Is_OTA_Port(port)) {
		OTA_Frame frame;
		OTA_Status status = OTA_Decode_Frame(bytes, (size_t)byte_count, &frame);
		if (status != OTA_STATUS_OK) {
			uint8_t transaction_id = byte_count >= 3 ? bytes[2] : 0;
			uint8_t request_command = byte_count >= 1 ? bytes[0] : 0;
			Send_OTA_Error(port, transaction_id, request_command, status);
			return;
		}
		Dispatch_OTA_Frame(port, &frame, settings_obj);
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
		cJSON* bridge_params = B100_Get_Params(NULL);
		cJSON_Delete(bridge_params);
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
	cJSON* root = cJSON_CreateObject();
	cJSON* counters_array = cJSON_CreateArray();
	cJSON* occupancy_array = cJSON_CreateArray();
	cJSON* presence_array = cJSON_CreateArray();
	Counting_Add_Counters_JSON(counters_array);
	Occupancy_Add_Status_JSON(occupancy_array);
	Presence_Add_Status_JSON(presence_array);
	
	cJSON_AddItemToObject(root, "counters", counters_array);
	cJSON_AddItemToObject(root, "occupancy", occupancy_array);
	cJSON_AddItemToObject(root, "presence", presence_array);
	time_t now = time(NULL);
	cJSON* publish = cJSON_CreateObject();
	Counting_Add_Publish_JSON(publish, now);
	Occupancy_Add_Publish_JSON(publish, now);
	Presence_Add_Publish_JSON(publish, now);
	cJSON_AddItemToObject(root, "publish", publish);

	pthread_mutex_lock(&g_publish_log_mutex);
	cJSON* publish_log = g_publish_log ? cJSON_Duplicate(g_publish_log, 1) : cJSON_CreateArray();
	cJSON_AddItemToObject(root, "publishLog", publish_log ? publish_log : cJSON_CreateArray());
	pthread_mutex_unlock(&g_publish_log_mutex);
	
	ACAP_HTTP_Respond_JSON(response, root);
	cJSON_Delete(root);
}

// ==================================================================
// LoRa Publishing — sends binary data directly via byte array
// ==================================================================

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
	int port = Counting_Port();
	int enabled = Counting_Enabled();
	
	if (!enabled) {
		LOG("Counting publish disabled\n");
		return 0;
	}
	if (!Counting_References_Ready())
		Initialize_Counting_References();

	unsigned char* buffer = NULL;
	size_t buffer_size = 0;
	int published_counter_count = 0;
	int total_class_count = 0;
	if (!Counting_Build_Payload(&buffer, &buffer_size, &published_counter_count, &total_class_count)) {
		return 0;
	}
	
	int use_confirmed = Next_Confirmed_Flag();
	LOG("LoRa: Publishing %zu counting bytes (%d counters, %d selected classes) on port %d%s\n",
	    buffer_size, published_counter_count, total_class_count, port, use_confirmed ? " [confirmed]" : "");

	int success = B100_Send_Bytes(buffer, (int)buffer_size, port, use_confirmed);
	if (success) {
		LOG("LoRa: Counting publish accepted\n");
		Record_Publish_Log(port, buffer, buffer_size);
		Counting_Mark_Published(time(NULL));
	} else {
		LOG_WARN("LoRa: Counting publish failed - %s\n", B100_Get_Last_Error());
	}
	
	free(buffer);
	return success;
}

static int Publish_Occupancy_To_LoRa() {
	int port = Occupancy_Port();
	int enabled = Occupancy_Enabled();
	
	if (!enabled) {
		LOG("Occupancy publish disabled\n");
		return 0;
	}
	unsigned char* buffer = NULL;
	size_t buffer_size = 0;
	int occupancy_sample_count = 0;
	int total_class_count = 0;
	if (!Occupancy_Build_Payload(&buffer, &buffer_size, &occupancy_sample_count, &total_class_count)) {
		return 0;
	}
	
	int use_confirmed = Next_Confirmed_Flag();
	LOG("LoRa: Publishing %zu occupancy bytes (%d scenarios, %d selected labels) on port %d%s\n",
	    buffer_size, occupancy_sample_count, total_class_count, port, use_confirmed ? " [confirmed]" : "");

	int success = B100_Send_Bytes(buffer, (int)buffer_size, port, use_confirmed);
	
	if (success) {
		LOG("LoRa: Occupancy publish accepted\n");
		Record_Publish_Log(port, buffer, buffer_size);
		Occupancy_Mark_Published(time(NULL));
	} else {
		LOG_WARN("LoRa: Occupancy publish failed - %s\n", B100_Get_Last_Error());
	}
	
	free(buffer);
	return success;
}

static int Publish_Presence_To_LoRa() {
	pthread_mutex_lock(&g_presence_publish_mutex);
	if (!Presence_Enabled()) {
		LOG("Presence publish disabled\n");
		pthread_mutex_unlock(&g_presence_publish_mutex);
		return 0;
	}
	unsigned char* buffer = NULL;
	size_t buffer_size = 0;
	int area_count = 0;
	unsigned long generation = 0;
	if (!Presence_Build_Payload(&buffer, &buffer_size, &area_count, &generation)) {
		pthread_mutex_unlock(&g_presence_publish_mutex);
		return 0;
	}

	int port = Presence_Port();
	int use_confirmed = Next_Confirmed_Flag();
	LOG("LoRa: Publishing %zu presence bytes (%d areas) on port %d%s\n",
	    buffer_size, area_count, port, use_confirmed ? " [confirmed]" : "");
	int success = B100_Send_Bytes(buffer, (int)buffer_size, port, use_confirmed);
	if (success) {
		LOG("LoRa: Presence publish accepted\n");
		Record_Publish_Log(port, buffer, buffer_size);
		Presence_Mark_Published(time(NULL), generation);
	} else {
		LOG_WARN("LoRa: Presence publish failed - %s\n", B100_Get_Last_Error());
	}
	free(buffer);
	pthread_mutex_unlock(&g_presence_publish_mutex);
	return success;
}

void*
Publish_Thread(void* arg) {
	LOG("Publish thread started\n");
	
	// Set initial next publish times
	Reset_Publish_Schedule();
	
	while (running) {
		Drain_OTA_Bridge_Config_Queue();
		Drain_OTA_Response_Queue();
		int counting_enabled = Counting_Enabled();
		int occupancy_enabled = Occupancy_Enabled();
		time_t next_counting_time = Counting_Next_Publish_Time();
		time_t next_occupancy_time = Occupancy_Next_Publish_Time();
		
		time_t now = time(NULL);
		Presence_Update(now);
		if (counting_enabled && now >= next_counting_time) {
			Publish_Counters_To_LoRa();
		}
		if (occupancy_enabled && now >= next_occupancy_time) {
			Publish_Occupancy_To_LoRa();
		}
		if (Presence_Should_Publish(now)) {
			LOG("Presence Alert state publish due\n");
			Publish_Presence_To_LoRa();
		}
		
		sleep(1);
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
	int request_presence = !stream || strcmp(stream, "presence") == 0 || strcmp(stream, "all") == 0;
	if (!request_counting && !request_occupancy && !request_presence) {
		if (stream) free(stream);
		ACAP_HTTP_Respond_Error(response, 400, "Invalid publish stream");
		return;
	}

	int attempted = 0;
	int success = 0;
	int counting_enabled = Counting_Enabled();
	int occupancy_enabled = Occupancy_Enabled();
	int presence_enabled = Presence_Enabled();

	if (request_counting && counting_enabled) {
		attempted++;
		if (Publish_Counters_To_LoRa()) success++;
	}
	if (request_occupancy && occupancy_enabled) {
		attempted++;
		if (Publish_Occupancy_To_LoRa()) success++;
	}
	if (request_presence && presence_enabled) {
		attempted++;
		if (Publish_Presence_To_LoRa()) success++;
	}
	if (stream) free(stream);
	
	if (attempted > 0 && success > 0) {
		ACAP_HTTP_Respond_Text(response, "Publish request sent");
	} else if (attempted == 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Selected publish stream is disabled");
	} else {
		const char* error = B100_Get_Last_Error();
		ACAP_HTTP_Respond_Error(response, 500,
		                        error && *error ? error : "Failed to publish selected stream");
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
		const char* raw = ACAP_HTTP_Get_Body(request);
		size_t raw_len = ACAP_HTTP_Get_Body_Length(request);
		if (raw && raw_len > 0) body = cJSON_ParseWithLength(raw, raw_len);
	}
	if (!body) {
		LOG_WARN("B100 status callback: failed to parse JSON body\n");
		ACAP_HTTP_Respond_Text(response, "OK");
		return;
	}

	char *json_str = cJSON_PrintUnformatted(body);
	if( json_str ) {
		LOG_TRACE("B100 status callback received JSON: %s\n", json_str);
		free(json_str);
	}

	ACAP_HTTP_Respond_Text(response, "OK");
	B100_Process_Status_Callback(body);
	cJSON_Delete(body);
}

void
HTTP_Endpoint_B100_Receive_Callback(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}

	const char* content_type = ACAP_HTTP_Get_Content_Type(request);
	const char* raw = ACAP_HTTP_Get_Body(request);
	size_t raw_len = ACAP_HTTP_Get_Body_Length(request);
	ACAP_STATUS_SetString("lorawan", "lastReceiveCallbackTime", ACAP_DEVICE_Local_Time());
	ACAP_STATUS_SetString("lorawan", "lastReceiveCallbackContentType", content_type ? content_type : "");
	ACAP_STATUS_SetNumber("lorawan", "lastReceiveCallbackBytes", (int)raw_len);

	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		if (raw && raw_len > 0) body = cJSON_ParseWithLength(raw, raw_len);
	}
	if (!body) {
		LOG_WARN("B100 receive callback: failed to parse JSON body (content-type=%s, bytes=%zu)\n",
		         content_type ? content_type : "", raw_len);
		ACAP_STATUS_SetString("lorawan", "lastReceiveCallbackResult", "invalid_json_acknowledged");
		ACAP_HTTP_Respond_Text(response, "OK");
		return;
	}

	char *json_str = cJSON_PrintUnformatted(body);
	if (json_str) {
		syslog(LOG_WARNING, "B100 receive callback: %s", json_str);
		LOG("B100 receive callback: %s\n", json_str);
		free(json_str);
	}

	ACAP_HTTP_Respond_Text(response, "OK");
	int processed = B100_Process_Receive_Callback(body);
	ACAP_STATUS_SetString("lorawan", "lastReceiveCallbackResult", processed ? "processed" : "received_no_payload");
	cJSON_Delete(body);
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
		const char* raw = ACAP_HTTP_Get_Body(request);
		size_t raw_len = ACAP_HTTP_Get_Body_Length(request);
		if (raw && raw_len > 0) body = cJSON_ParseWithLength(raw, raw_len);
	}
	if (!body) {
		LOG_WARN("B100 GPS callback: failed to parse JSON body\n");
		ACAP_HTTP_Respond_Text(response, "OK");
		return;
	}

	char* json_str = cJSON_PrintUnformatted(body);
	if (json_str) {
		LOG_TRACE("B100 GPS callback received JSON: %s\n", json_str);
		free(json_str);
	}

	ACAP_HTTP_Respond_Text(response, "OK");
	B100_Process_GPS_Callback(body);
	cJSON_Delete(body);
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
	Counting_Delete_By_Scenario(scenario);
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
	LOG("Current backend has %d counters before sync\n", Counting_Count());
	
	Counting_Sync_With_AOA_List(scenarios);
	
	cJSON_Delete(body);
	
	// Return current counter count
	cJSON* result = cJSON_CreateObject();
	cJSON_AddNumberToObject(result, "removed", -1);  // Will be calculated in sync
	cJSON_AddNumberToObject(result, "remaining", Counting_Count());
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

	if (!Counting_Set_Values_From_JSON(body)) {
		cJSON_Delete(body);
		ACAP_HTTP_Respond_Error(response, 404, "Scenario not found or missing scenario name");
		return;
	}
	cJSON_Delete(body);

	ACAP_HTTP_Respond_Text(response, "Counter values updated");
}

void
HTTP_Endpoint_translator(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	char* inline_param = ACAP_HTTP_Request_Param(request, "inline");
	int inline_mode = inline_param && (strcmp(inline_param, "1") == 0 || strcmp(inline_param, "true") == 0 || strcmp(inline_param, "yes") == 0);

	int counting_port = LORA_PORT_COUNTING;
	int occupancy_port = LORA_PORT_OCCUPANCY;
	int presence_port = LORA_PORT_PRESENCE;
	cJSON* counter_defs = cJSON_CreateArray();
	cJSON* occupancy_defs = cJSON_CreateArray();
	cJSON* presence_defs = cJSON_CreateArray();
	Counting_Build_Decoder_Definitions(counter_defs, counting_port);
	Occupancy_Build_Decoder_Definitions(occupancy_defs, occupancy_port);
	Presence_Build_Decoder_Definitions(presence_defs, presence_port);
	char* js_counter_defs = cJSON_PrintUnformatted(counter_defs);
	char* js_occupancy_defs = cJSON_PrintUnformatted(occupancy_defs);
	char* js_presence_defs = cJSON_PrintUnformatted(presence_defs);
	if (!js_counter_defs) js_counter_defs = strdup("[]");
	if (!js_occupancy_defs) js_occupancy_defs = strdup("[]");
	if (!js_presence_defs) js_presence_defs = strdup("[]");

	GString* js_builder = g_string_sized_new(32768);
	if (!js_builder) {
		free(js_counter_defs);
		free(js_occupancy_defs);
		free(js_presence_defs);
		cJSON_Delete(counter_defs);
		cJSON_Delete(occupancy_defs);
		cJSON_Delete(presence_defs);
		ACAP_HTTP_Respond_Error(response, 500, "Failed to allocate JavaScript translator");
		return;
	}
	
	const char* dev_model  = ACAP_DEVICE_Prop("model")  ? ACAP_DEVICE_Prop("model")  : "unknown";
	const char* dev_serial = ACAP_DEVICE_Prop("serial") ? ACAP_DEVICE_Prop("serial") : "000000";
	const char* dev_date   = ACAP_DEVICE_Date();

	g_string_append_printf(js_builder,
		"/**\n"
		" * AI-B100 JavaScript Translator / Decoder\n"
		" * Device  : %s  (serial %s)\n"
		" * Generated: %s\n"
		" * Counting port: %d\n"
		" * Occupancy port: %d\n"
		" * Presence Alert port: %d\n"
		" *\n"
		" * Quick use:\n"
		" *   1) Set payloadPort to the LoRaWAN fPort (1, 2, or 3).\n"
		" *   2) Set payloadInput to an even-length hexadecimal string, for example '0001'.\n"
		" *   3) Call decodeByPort(payloadPort, payloadInput).\n"
		" *\n"
		" * Node-RED after converting the LoRaWAN data to a hex string:\n"
		" *   msg.payload = JavaScriptTranslator(msg.topic, msg.payload);\n"
		" *   return msg;\n"
		" *\n"
		" * Counting, Occupancy, and Presence Alert ports are fixed protocol ports. Download a new decoder after changing labels, scenes, or value types.\n"
		" */\n\n",
		dev_model, dev_serial, dev_date,
		counting_port, occupancy_port, presence_port);

	g_string_append_printf(js_builder,
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
		"function hexToBytes(hex) {\n"
		"  if (typeof hex !== 'string') return null;\n"
		"  var text = hex.trim();\n"
		"  if ((text.length %% 2) !== 0 || !/^[0-9A-Fa-f]*$/.test(text)) return null;\n"
		"  var bytes = [];\n"
		"  for (var i = 0; i < text.length; i += 2) bytes.push(parseInt(text.slice(i, i + 2), 16));\n"
		"  return bytes;\n"
		"}\n\n"
		"var countingPort = %d;\n"
		"var counterScenarios = %s;\n"
		"var occupancyPort = %d;\n"
		"var occupancyScenarios = %s;\n"
		"var occupancyValueTypes = { 0: 'max', 1: 'min', 2: 'avg' };\n"
		"var presencePort = %d;\n"
		"var presenceAreas = %s;\n\n",
		counting_port, js_counter_defs, occupancy_port, js_occupancy_defs, presence_port, js_presence_defs);

	g_string_append_printf(js_builder,
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

	g_string_append_printf(js_builder,
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

	g_string_append_printf(js_builder,
			"/**\n"
			" * Presence Alert decoder\n"
			" * Each configured Presence scene contributes exactly one state byte in configured order.\n"
			" * Selected classes only define which detected objects can trigger that scene; classes do not have separate bytes.\n"
			" * 0 means no matching presence and 1 means presence matched the scene threshold.\n"
			" */\n"
			"function decodePresence(bytes) {\n"
			"  var result = {};\n"
			"  var expectedLength = presenceAreas.length;\n"
			"  if (!bytes || bytes.length !== expectedLength) return { error: 'Invalid Presence Alert payload length', expected: expectedLength, bytes: bytes ? bytes.length : 0 };\n"
			"  for (var i = 0; i < presenceAreas.length; i++) {\n"
			"    var area = presenceAreas[i];\n"
			"    var key = safeKey(area ? area.name : ('unknown_' + i));\n"
			"    result[key] = { presence: bytes[i] !== 0 };\n"
			"  }\n"
			"  return result;\n"
			"}\n\n");

	g_string_append(js_builder, "function decodeByPort(port, hexPayload) {\n");
	g_string_append(js_builder, "  port = Number(port);\n");
	g_string_append(js_builder, "  var bytes = hexToBytes(hexPayload);\n");
	g_string_append(js_builder, "  if (!bytes) return { error: 'Payload must be an even-length hexadecimal string' };\n");
	g_string_append(js_builder, "  if (port === countingPort) return decodeCounting(bytes);\n");
	g_string_append(js_builder, "  if (port === occupancyPort) return decodeOccupancy(bytes);\n");
	g_string_append(js_builder, "  if (port === presencePort) return decodePresence(bytes);\n");
	g_string_append(js_builder,
		"  return { error: 'Unsupported port ' + port, port: port };\n"
		"}\n");
	g_string_append(js_builder,
		"\n"
		"function JavaScriptTranslator(port, hexPayload) {\n"
		"  return decodeByPort(port, hexPayload);\n"
		"}\n\n"
		"function Decode(fPort, payload) {\n"
		"  return decodeByPort(fPort, payload);\n"
		"}\n\n"
		"function Decoder(hexPayload, port) {\n"
		"  return decodeByPort(port, hexPayload);\n"
		"}\n\n"
		"// Fill these before calling decodeByPort(...)\n"
		"var port = null; //Number\n"
		"var message = null; //Hex-encoded string\n"
		"var decodedMessage = decodeByPort(port, message);\n");
	free(js_counter_defs);
	free(js_occupancy_defs);
	free(js_presence_defs);
	cJSON_Delete(counter_defs);
	cJSON_Delete(occupancy_defs);
	cJSON_Delete(presence_defs);
	const char* js = js_builder->str;
	
	// Build filename: aib100-decoder-{model}-{serial}-{date}.js
	char filename[128];
	snprintf(filename, sizeof(filename), "aib100-decoder-%s-%s-%s.js",
	         dev_model, dev_serial, dev_date);
	size_t js_length = strlen(js);

	if (inline_mode) {
		ACAP_HTTP_Respond_String(response,
			"Content-Type: application/javascript; charset=utf-8\r\n"
			"Cache-Control: no-cache\r\n"
			"Content-Length: %zu\r\n\r\n",
			js_length);
	} else {
		// Send as downloadable JavaScript file
		ACAP_HTTP_Header_FILE(response, filename, "application/javascript", js_length);
	}
	if (!ACAP_HTTP_Respond_Data(response, js_length, js))
		LOG_WARN("Failed to write JavaScript translator response (%zu bytes)\n", js_length);
	g_string_free(js_builder, TRUE);
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
	Counting_Load_From_File();
    
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

	Initialize_Counting_References();
    
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
			cJSON* apiuser = cJSON_GetObjectItem(b100, "apiDigestUser");
			if (apiuser && apiuser->valuestring) {
				strncpy(g_b100_api_user, apiuser->valuestring, sizeof(g_b100_api_user) - 1);
				g_b100_api_user[sizeof(g_b100_api_user) - 1] = '\0';
			}
			cJSON* apipass = cJSON_GetObjectItem(b100, "apiDigestPassword");
			if (apipass && apipass->valuestring) {
				strncpy(g_b100_api_password, apipass->valuestring, sizeof(g_b100_api_password) - 1);
				g_b100_api_password[sizeof(g_b100_api_password) - 1] = '\0';
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
			cJSON* presence = cJSON_GetObjectItem(transmission, "presence");
			if (counting || occupancy || presence) {
				Counting_Load_Config(counting, LORA_PORT_COUNTING);
				Occupancy_Load_Config(occupancy, LORA_PORT_OCCUPANCY);
				Presence_Load_Config(presence, LORA_PORT_PRESENCE);
			} else {
				Counting_Load_Config(transmission, LORA_PORT_COUNTING);
				cJSON* old_value = cJSON_GetObjectItem(transmission, "occupancyValue");
				if (old_value && old_value->valuestring && Occupancy_Is_Valid_Value(old_value->valuestring)) {
					cJSON* legacy_occupancy = cJSON_CreateObject();
					if (legacy_occupancy) {
						cJSON_AddStringToObject(legacy_occupancy, "value", old_value->valuestring);
						Occupancy_Load_Config(legacy_occupancy, LORA_PORT_OCCUPANCY);
						cJSON_Delete(legacy_occupancy);
					}
				}
			}
			Presence_Initialize_State();
        }
    }
    
    // Initialize B100 client
    LOG("Initializing B100 client: %s:%d\n", g_b100_ip, g_b100_port);
    B100_Init(g_b100_ip, g_b100_port, 30);
		if (!B100_Set_API_Credentials(g_b100_api_user, g_b100_api_password))
		LOG_WARN("Bridge API credentials must both be set or both be empty\n");
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
	ACAP_HTTP_Node("ota_encoder", HTTP_Endpoint_OTA_Encoder);
	ACAP_HTTP_Node("ota_decoder", HTTP_Endpoint_OTA_Decoder);
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
	Reset_Publish_Schedule();
    
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
	pthread_mutex_lock(&g_ota_response_mutex);
	if (g_ota_response_queue) {
		g_queue_free_full(g_ota_response_queue, free);
		g_ota_response_queue = NULL;
	}
	if (g_ota_bridge_config_queue) {
		g_queue_free_full(g_ota_bridge_config_queue, free);
		g_ota_bridge_config_queue = NULL;
	}
	pthread_mutex_unlock(&g_ota_response_mutex);
	
	// Save counters before exit
	LOG("Saving counters before shutdown...\n");
	Counting_Save_To_File();
	
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
