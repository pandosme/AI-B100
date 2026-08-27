#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "ACAP.h"
#include "presence.h"

#define PRESENCE_MAX_AREAS 10
#define LOG(fmt, args...) { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args); }
#define LOG_WARN(fmt, args...) { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args); }

typedef struct {
	char scenario[64];
	int scenario_id;
	int trigger_delay;
	int known;
	int aoa_active;
	int active;
	time_t last_update;
	time_t clear_due;
} PresenceArea;

static PresenceArea g_areas[PRESENCE_MAX_AREAS];
static int g_area_count = 0;
static int g_enabled = 0;
static int g_port = 3;
static int g_pending = 0;
static unsigned long g_state_generation = 0;
static time_t g_last_publish_time = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static int Parse_Active(cJSON* item, int* active) {
	if (!item || !active) return 0;
	if (cJSON_IsBool(item)) {
		*active = cJSON_IsTrue(item);
		return 1;
	}
	if (cJSON_IsNumber(item)) {
		*active = item->valueint != 0;
		return 1;
	}
	if (cJSON_IsString(item) && item->valuestring) {
		if (strcmp(item->valuestring, "1") == 0 || strcmp(item->valuestring, "true") == 0) {
			*active = 1;
			return 1;
		}
		if (strcmp(item->valuestring, "0") == 0 || strcmp(item->valuestring, "false") == 0) {
			*active = 0;
			return 1;
		}
	}
	return 0;
}

static int Is_Occupancy_Type(const char* type) {
	return type && (strcmp(type, "occupancyInArea") == 0 || strcmp(type, "OccupancyInArea") == 0);
}

static int Find_Area(const char* scenario) {
	if (!scenario) return -1;
	for (int i = 0; i < g_area_count; i++) {
		if (strcmp(g_areas[i].scenario, scenario) == 0) return i;
	}
	return -1;
}

static int Find_Area_By_ID(int scenario_id) {
	for (int i = 0; i < g_area_count; i++) {
		if (g_areas[i].scenario_id == scenario_id) return i;
	}
	return -1;
}

static int Threshold_Scenario_ID(cJSON* event) {
	cJSON* event_topic = cJSON_GetObjectItem(event, "event");
	if (!event_topic || !cJSON_IsString(event_topic) || !event_topic->valuestring) return -1;
	const char* marker = strstr(event_topic->valuestring, "Scenario");
	if (!marker) return -1;
	char* end = NULL;
	long scenario_id = strtol(marker + strlen("Scenario"), &end, 10);
	if (end == marker + strlen("Scenario") || strcmp(end, "Threshold") != 0 ||
	    scenario_id < 0 || scenario_id > INT_MAX)
		return -1;
	return (int)scenario_id;
}

static int Has_Enabled_Areas(void) {
	return g_area_count > 0;
}

static void Queue_Publish(void) {
	g_pending = 1;
	g_state_generation++;
}

static int Scenario_Is_Presence_Alert(cJSON* scenario) {
	cJSON* type = cJSON_GetObjectItem(scenario, "type");
	return type && cJSON_IsString(type) && Is_Occupancy_Type(type->valuestring);
}

static cJSON* Legacy_Time_In_Area_Condition(cJSON* scenario) {
	cJSON* type = cJSON_GetObjectItem(scenario, "type");
	if (!type || !cJSON_IsString(type) ||
	    (strcmp(type->valuestring, "motion") != 0 && strcmp(type->valuestring, "Motion") != 0))
		return NULL;
	cJSON* trigger = NULL;
	cJSON_ArrayForEach(trigger, cJSON_GetObjectItem(scenario, "triggers")) {
		cJSON* condition = NULL;
		cJSON_ArrayForEach(condition, cJSON_GetObjectItem(trigger, "conditions")) {
			cJSON* condition_type = cJSON_GetObjectItem(condition, "type");
			if (condition_type && cJSON_IsString(condition_type) &&
			    strcmp(condition_type->valuestring, "individualTimeInArea") == 0)
				return condition;
		}
	}
	return NULL;
}

static void Replace_JSON_Item(cJSON* object, const char* key, cJSON* replacement) {
	if (cJSON_GetObjectItem(object, key)) cJSON_ReplaceItemInObject(object, key, replacement);
	else cJSON_AddItemToObject(object, key, replacement);
}

static void Convert_Legacy_Scenario(cJSON* scenario) {
	Replace_JSON_Item(scenario, "type", cJSON_CreateString("occupancyInArea"));
	cJSON* converted_triggers = cJSON_CreateArray();
	cJSON* trigger = NULL;
	cJSON_ArrayForEach(trigger, cJSON_GetObjectItem(scenario, "triggers")) {
		cJSON* trigger_type = cJSON_GetObjectItem(trigger, "type");
		if (!trigger_type || !cJSON_IsString(trigger_type) || strcmp(trigger_type->valuestring, "includeArea") != 0) continue;
		cJSON* converted = cJSON_Duplicate(trigger, 1);
		cJSON_DeleteItemFromObject(converted, "conditions");
		cJSON_AddItemToArray(converted_triggers, converted);
	}
	Replace_JSON_Item(scenario, "triggers", converted_triggers);
	Replace_JSON_Item(scenario, "filters", cJSON_CreateArray());
	cJSON* event_interval = cJSON_CreateObject();
	cJSON_AddBoolToObject(event_interval, "enabled", 0);
	Replace_JSON_Item(scenario, "eventInterval", event_interval);
	cJSON* threshold = cJSON_CreateObject();
	cJSON_AddBoolToObject(threshold, "enabled", 1);
	cJSON_AddNumberToObject(threshold, "triggerDelay", 0);
	cJSON* thresholds = cJSON_CreateArray();
	cJSON* more_than_zero = cJSON_CreateObject();
	cJSON_AddNumberToObject(more_than_zero, "level", 0);
	cJSON_AddStringToObject(more_than_zero, "type", "moreThan");
	cJSON_AddItemToArray(thresholds, more_than_zero);
	cJSON_AddItemToObject(threshold, "thresholds", thresholds);
	Replace_JSON_Item(scenario, "thresholdConfiguration", threshold);
}

static int Ensure_Threshold_Event_Configuration(cJSON* scenario) {
	int changed = 0;
	cJSON* event_interval = cJSON_GetObjectItem(scenario, "eventInterval");
	cJSON* interval_enabled = event_interval ? cJSON_GetObjectItem(event_interval, "enabled") : NULL;
	if (!event_interval || !cJSON_IsObject(event_interval) || !interval_enabled || !cJSON_IsFalse(interval_enabled)) {
		event_interval = cJSON_CreateObject();
		cJSON_AddBoolToObject(event_interval, "enabled", 0);
		Replace_JSON_Item(scenario, "eventInterval", event_interval);
		changed = 1;
	}
	cJSON* threshold = cJSON_GetObjectItem(scenario, "thresholdConfiguration");
	cJSON* enabled = threshold ? cJSON_GetObjectItem(threshold, "enabled") : NULL;
	cJSON* delay = threshold ? cJSON_GetObjectItem(threshold, "triggerDelay") : NULL;
	cJSON* thresholds = threshold ? cJSON_GetObjectItem(threshold, "thresholds") : NULL;
	cJSON* first = thresholds && cJSON_IsArray(thresholds) ? cJSON_GetArrayItem(thresholds, 0) : NULL;
	cJSON* level = first ? cJSON_GetObjectItem(first, "level") : NULL;
	cJSON* type = first ? cJSON_GetObjectItem(first, "type") : NULL;
	if (!threshold || !cJSON_IsObject(threshold) || !enabled || !cJSON_IsTrue(enabled) ||
	    !delay || !cJSON_IsNumber(delay) || delay->valueint < 0 ||
	    !thresholds || cJSON_GetArraySize(thresholds) != 1 ||
	    !level || !cJSON_IsNumber(level) || level->valueint < 0 ||
	    !type || !cJSON_IsString(type) || strcmp(type->valuestring, "moreThan") != 0) {
		threshold = cJSON_CreateObject();
		cJSON_AddBoolToObject(threshold, "enabled", 1);
		cJSON_AddNumberToObject(threshold, "triggerDelay", 5);
		thresholds = cJSON_CreateArray();
		first = cJSON_CreateObject();
		cJSON_AddNumberToObject(first, "level", 0);
		cJSON_AddStringToObject(first, "type", "moreThan");
		cJSON_AddItemToArray(thresholds, first);
		cJSON_AddItemToObject(threshold, "thresholds", thresholds);
		Replace_JSON_Item(scenario, "thresholdConfiguration", threshold);
		changed = 1;
	}
	return changed;
}

static int Apply_AOA_Configuration(cJSON* data) {
	cJSON* request = cJSON_CreateObject();
	cJSON_AddStringToObject(request, "apiVersion", "1.6");
	cJSON_AddStringToObject(request, "context", "aib100-presence-migration");
	cJSON_AddStringToObject(request, "method", "setConfiguration");
	cJSON_AddItemToObject(request, "params", cJSON_Duplicate(data, 1));
	char* request_text = cJSON_PrintUnformatted(request);
	cJSON_Delete(request);
	if (!request_text) return 0;
	char* response_text = ACAP_VAPIX_Post("/local/objectanalytics/control.cgi", request_text);
	free(request_text);
	cJSON* response = response_text ? cJSON_Parse(response_text) : NULL;
	int success = response && !cJSON_GetObjectItem(response, "error");
	if (!success) LOG_WARN("Failed to migrate Presence Alert AOA configuration: %s\n", response_text ? response_text : "no response");
	if (response) cJSON_Delete(response);
	free(response_text);
	return success;
}

void Presence_Load_Config(cJSON* config, int fixed_port) {
	if (!config) return;
	PresenceArea previous[PRESENCE_MAX_AREAS];
	int previous_count;

	pthread_mutex_lock(&g_mutex);
	int pending = g_pending;
	g_state_generation++;
	memcpy(previous, g_areas, sizeof(previous));
	previous_count = g_area_count;
	memset(g_areas, 0, sizeof(g_areas));
	g_area_count = 0;

	cJSON* enabled = cJSON_GetObjectItem(config, "enabled");
	if (enabled) g_enabled = cJSON_IsTrue(enabled);
	g_port = fixed_port;

	cJSON* scenarios = cJSON_GetObjectItem(config, "scenarios");
	for (cJSON* item = scenarios && cJSON_IsObject(scenarios) ? scenarios->child : NULL;
	     item && g_area_count < PRESENCE_MAX_AREAS; item = item->next) {
		if (!item->string || !cJSON_IsObject(item)) continue;
		PresenceArea* area = &g_areas[g_area_count++];
		strncpy(area->scenario, item->string, sizeof(area->scenario) - 1);
		area->scenario_id = -1;
		for (int i = 0; i < previous_count; i++) {
			if (strcmp(previous[i].scenario, area->scenario) == 0) {
				area->scenario_id = previous[i].scenario_id;
				area->trigger_delay = previous[i].trigger_delay;
				area->known = previous[i].known;
				area->aoa_active = previous[i].aoa_active;
				area->active = previous[i].active;
				area->last_update = previous[i].last_update;
				area->clear_due = previous[i].clear_due;
				break;
			}
		}
	}
	g_pending = pending;
	if (!g_enabled) g_pending = 0;
	pthread_mutex_unlock(&g_mutex);
	Presence_Update_ACAP_Status();
}

void Presence_Initialize_State(void) {
	char scenarios[PRESENCE_MAX_AREAS][64];
	int scenario_count = 0;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < g_area_count; i++) {
		memcpy(scenarios[scenario_count], g_areas[i].scenario, sizeof(scenarios[scenario_count]));
		scenario_count++;
	}
	pthread_mutex_unlock(&g_mutex);
	if (scenario_count == 0) {
		Presence_Update_ACAP_Status();
		return;
	}

	const char* endpoint = "/local/objectanalytics/control.cgi";
	const char* config_request =
		"{\"apiVersion\":\"1.6\",\"context\":\"aib100-presence\",\"method\":\"getConfiguration\"}";
	char* config_text = ACAP_VAPIX_Post(endpoint, config_request);
	cJSON* config_response = config_text ? cJSON_Parse(config_text) : NULL;
	cJSON* data = config_response ? cJSON_GetObjectItem(config_response, "data") : NULL;
	cJSON* configured_scenarios = data ? cJSON_GetObjectItem(data, "scenarios") : NULL;
	int configuration_changed = 0;
	cJSON* scenario = NULL;
	cJSON_ArrayForEach(scenario, configured_scenarios) {
		cJSON* name = cJSON_GetObjectItem(scenario, "name");
		if (!name || !cJSON_IsString(name)) continue;
		for (int i = 0; i < scenario_count; i++) {
			if (strcmp(name->valuestring, scenarios[i]) != 0) continue;
			if (Legacy_Time_In_Area_Condition(scenario)) {
				Convert_Legacy_Scenario(scenario);
				LOG("Migrating Presence Alert %s to total-based Occupancy In Area\n", name->valuestring);
				configuration_changed = 1;
			}
			if (Scenario_Is_Presence_Alert(scenario) && Ensure_Threshold_Event_Configuration(scenario))
				configuration_changed = 1;
		}
	}
	if (configuration_changed && !Apply_AOA_Configuration(data)) {
		if (config_response) cJSON_Delete(config_response);
		free(config_text);
		Presence_Update_ACAP_Status();
		return;
	}

	for (int i = 0; i < scenario_count; i++) {
		int found = 0;
		int scenario_id = -1;
		int trigger_delay = 0;
		cJSON_ArrayForEach(scenario, configured_scenarios) {
			cJSON* name = cJSON_GetObjectItem(scenario, "name");
			if (name && cJSON_IsString(name) && strcmp(name->valuestring, scenarios[i]) == 0 &&
			    Scenario_Is_Presence_Alert(scenario)) {
				found = 1;
				cJSON* id = cJSON_GetObjectItem(scenario, "id");
				if (id && cJSON_IsNumber(id)) scenario_id = id->valueint;
				cJSON* threshold = cJSON_GetObjectItem(scenario, "thresholdConfiguration");
				cJSON* delay = threshold ? cJSON_GetObjectItem(threshold, "triggerDelay") : NULL;
				if (delay && cJSON_IsNumber(delay) && delay->valueint >= 0) trigger_delay = delay->valueint;
				break;
			}
		}
		if (!found) {
			LOG_WARN("Presence Alert Occupancy In Area scene not found: %s\n", scenarios[i]);
			continue;
		}
		pthread_mutex_lock(&g_mutex);
		int index = Find_Area(scenarios[i]);
		if (index >= 0) {
			g_areas[index].scenario_id = scenario_id;
			g_areas[index].trigger_delay = trigger_delay;
		}
		pthread_mutex_unlock(&g_mutex);
	}

	if (config_response) cJSON_Delete(config_response);
	free(config_text);
	Presence_Update_ACAP_Status();
}

void Presence_Process_AOA_Event(cJSON* event) {
	int scenario_id = Threshold_Scenario_ID(event);
	if (scenario_id < 0) return;
	int aoa_active = 0;
	if (!Parse_Active(cJSON_GetObjectItem(event, "active"), &aoa_active)) {
		char* event_json = cJSON_PrintUnformatted(event);
		LOG_WARN("Presence Alert threshold event missing valid active state: %s\n", event_json ? event_json : "{}");
		free(event_json);
		return;
	}
	time_t now = time(NULL);
	int changed = 0;
	int internal_active = 0;
	char scenario[64] = "";
	pthread_mutex_lock(&g_mutex);
	int index = Find_Area_By_ID(scenario_id);
	if (index >= 0) {
		PresenceArea* area = &g_areas[index];
		strncpy(scenario, area->scenario, sizeof(scenario) - 1);
		int initial = !area->known;
		area->known = 1;
		area->aoa_active = aoa_active;
		area->last_update = now;
		if (aoa_active) {
			area->clear_due = 0;
			if (!area->active) {
				area->active = 1;
				changed = 1;
			}
		} else {
			if (area->active && area->trigger_delay == 0) {
				area->active = 0;
				area->clear_due = 0;
				changed = 1;
			} else {
				area->clear_due = area->active ? now + (2 * area->trigger_delay) : 0;
			}
		}
		if (initial) changed = 1;
		internal_active = area->active;
		if (changed && g_enabled) Queue_Publish();
	}
	pthread_mutex_unlock(&g_mutex);
	if (index < 0) return;
	LOG("Presence Alert threshold %s (id %d): aoa=%d, active=%d\n", scenario, scenario_id, aoa_active, internal_active);
	Presence_Update_ACAP_Status();
}

void Presence_Update(time_t now) {
	int changed = 0;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < g_area_count; i++) {
		PresenceArea* area = &g_areas[i];
		if (area->active && !area->aoa_active && area->clear_due > 0 && now >= area->clear_due) {
			area->active = 0;
			area->clear_due = 0;
			changed = 1;
			if (g_enabled) Queue_Publish();
			LOG("Presence Alert retained clear elapsed: %s\n", area->scenario);
		}
	}
	pthread_mutex_unlock(&g_mutex);
	if (changed) Presence_Update_ACAP_Status();
}

int Presence_Enabled(void) {
	pthread_mutex_lock(&g_mutex);
	int enabled = g_enabled;
	pthread_mutex_unlock(&g_mutex);
	return enabled;
}

int Presence_Port(void) {
	pthread_mutex_lock(&g_mutex);
	int port = g_port;
	pthread_mutex_unlock(&g_mutex);
	return port;
}

int Presence_Should_Publish(time_t now) {
	(void)now;
	pthread_mutex_lock(&g_mutex);
	int due = g_enabled && Has_Enabled_Areas() && g_pending;
	pthread_mutex_unlock(&g_mutex);
	return due;
}

time_t Presence_Next_Publish_Time(time_t now) {
	pthread_mutex_lock(&g_mutex);
	time_t next = 0;
	if (g_enabled && Has_Enabled_Areas()) {
		if (g_pending) next = now;
		for (int i = 0; i < g_area_count; i++) {
			if (g_areas[i].clear_due > 0 && (next == 0 || g_areas[i].clear_due < next))
				next = g_areas[i].clear_due;
		}
	}
	pthread_mutex_unlock(&g_mutex);
	return next;
}

void Presence_Mark_Published(time_t now, unsigned long generation) {
	pthread_mutex_lock(&g_mutex);
	if (generation == g_state_generation) g_pending = 0;
	g_last_publish_time = now;
	pthread_mutex_unlock(&g_mutex);
	Presence_Update_ACAP_Status();
}

void Presence_Reset_Timers(void) {
	pthread_mutex_lock(&g_mutex);
	g_last_publish_time = 0;
	g_pending = 0;
	pthread_mutex_unlock(&g_mutex);
}

void Presence_Reset_All(void) {
	pthread_mutex_lock(&g_mutex);
	for (int index = 0; index < g_area_count; index++) {
		g_areas[index].known = 0;
		g_areas[index].aoa_active = 0;
		g_areas[index].active = 0;
		g_areas[index].last_update = 0;
		g_areas[index].clear_due = 0;
	}
	g_pending = 0;
	g_last_publish_time = 0;
	g_state_generation++;
	pthread_mutex_unlock(&g_mutex);
	Presence_Update_ACAP_Status();
}

void Presence_Update_ACAP_Status(void) {
	cJSON* areas = cJSON_CreateArray();
	if (!areas) return;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < g_area_count; i++) {
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "scenario", g_areas[i].scenario);
		cJSON_AddNumberToObject(item, "scenarioId", g_areas[i].scenario_id);
		cJSON_AddNumberToObject(item, "triggerDelay", g_areas[i].trigger_delay);
		cJSON_AddBoolToObject(item, "known", g_areas[i].known);
		cJSON_AddBoolToObject(item, "aoaActive", g_areas[i].aoa_active);
		cJSON_AddBoolToObject(item, "active", g_areas[i].active);
		cJSON_AddNumberToObject(item, "lastUpdate", (double)g_areas[i].last_update);
		cJSON_AddNumberToObject(item, "clearDue", (double)g_areas[i].clear_due);
		cJSON_AddItemToArray(areas, item);
	}
	cJSON* summary = cJSON_CreateObject();
	cJSON_AddBoolToObject(summary, "enabled", g_enabled);
	cJSON_AddNumberToObject(summary, "port", g_port);
	cJSON_AddBoolToObject(summary, "pending", g_pending);
	cJSON_AddNumberToObject(summary, "lastPublishTime", (double)g_last_publish_time);
	cJSON_AddItemToObject(summary, "areas", areas);
	pthread_mutex_unlock(&g_mutex);
	ACAP_STATUS_SetObject("presence", "state", summary);
	cJSON_Delete(summary);
}

void Presence_Add_Status_JSON(cJSON* presence_array) {
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < g_area_count; i++) {
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "scenario", g_areas[i].scenario);
		cJSON_AddNumberToObject(item, "scenarioId", g_areas[i].scenario_id);
		cJSON_AddBoolToObject(item, "known", g_areas[i].known);
		cJSON_AddBoolToObject(item, "aoaActive", g_areas[i].aoa_active);
		cJSON_AddBoolToObject(item, "active", g_areas[i].active);
		cJSON_AddNumberToObject(item, "lastUpdate", (double)g_areas[i].last_update);
		cJSON_AddNumberToObject(item, "clearDue", (double)g_areas[i].clear_due);
		cJSON_AddItemToArray(presence_array, item);
	}
	pthread_mutex_unlock(&g_mutex);
}

void Presence_Add_Publish_JSON(cJSON* publish, time_t now) {
	pthread_mutex_lock(&g_mutex);
	cJSON* presence = cJSON_CreateObject();
	time_t next = g_pending ? now : 0;
	for (int i = 0; i < g_area_count; i++) {
		if (g_areas[i].clear_due > 0 && (next == 0 || g_areas[i].clear_due < next))
			next = g_areas[i].clear_due;
	}
	cJSON_AddBoolToObject(presence, "enabled", g_enabled);
	cJSON_AddNumberToObject(presence, "port", g_port);
	cJSON_AddBoolToObject(presence, "pending", g_pending);
	cJSON_AddNumberToObject(presence, "nextPublishTime", (double)next);
	cJSON_AddNumberToObject(presence, "secondsUntilPublish", next > now ? (int)(next - now) : 0);
	pthread_mutex_unlock(&g_mutex);
	cJSON_AddItemToObject(publish, "presence", presence);
}

void Presence_Build_Decoder_Definitions(cJSON* definitions, int port) {
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < g_area_count; i++) {
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "name", g_areas[i].scenario);
		cJSON_AddNumberToObject(item, "port", port);
		cJSON_AddItemToArray(definitions, item);
	}
	pthread_mutex_unlock(&g_mutex);
}

int Presence_Build_Payload(unsigned char** out_buffer, size_t* out_size, int* out_area_count, unsigned long* out_generation) {
	if (!out_buffer || !out_size || !out_area_count || !out_generation) return 0;
	*out_buffer = NULL;
	*out_size = 0;
	*out_area_count = 0;
	*out_generation = 0;
	pthread_mutex_lock(&g_mutex);
	if (!Has_Enabled_Areas()) {
		pthread_mutex_unlock(&g_mutex);
		LOG_WARN("No Presence Alert scenes selected; payload deferred\n");
		return 0;
	}
	int count = 0;
	for (int i = 0; i < g_area_count; i++) count++;
	unsigned char* buffer = malloc((size_t)count);
	if (!buffer) {
		pthread_mutex_unlock(&g_mutex);
		return 0;
	}
	int offset = 0;
	for (int i = 0; i < g_area_count; i++) {
		buffer[offset++] = g_areas[i].active ? 1 : 0;
	}
	unsigned long generation = g_state_generation;
	pthread_mutex_unlock(&g_mutex);
	*out_buffer = buffer;
	*out_size = (size_t)count;
	*out_area_count = count;
	*out_generation = generation;
	return 1;
}