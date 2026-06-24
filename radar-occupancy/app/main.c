#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <glib.h>
#include <glib-unix.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ACAP.h"
#include "cJSON.h"
#include "B100.h"
#include "RadarDetection.h"

#define APP_PACKAGE	"aib100"
#define B100_STATUS_CALLBACK_NODE "b100_status"
#define B100_RECEIVE_CALLBACK_NODE "b100_receive"
#define B100_GPS_CALLBACK_NODE "b100_gps"
#define DOWNLINK_LOG_MAX 10
#define RADAR_MAX_OBJECTS 128
#define RADAR_MAX_POLYGON_POINTS 16
#define RADAR_MODE_MAXIMUM 0
#define RADAR_MODE_ENTRY_EXIT 1
#define RADAR_MODE_ALERT 2

#define LOG(fmt, args...)    { syslog(LOG_INFO, fmt, ## args); printf(fmt, ## args);}
#define LOG_WARN(fmt, args...)    { syslog(LOG_WARNING, fmt, ## args); printf(fmt, ## args);}
#define LOG_TRACE(fmt, args...)    { syslog(LOG_INFO, "TRACE: " fmt, ## args); printf("TRACE: " fmt, ## args); }

// Global state
static GMainLoop *main_loop = NULL;
static pthread_t health_thread;
static pthread_t publish_thread;
static int running = 1;
static int g_callbacks_configured = 0;
static cJSON* g_downlink_log = NULL;

// Transmission settings
static int g_publish_interval_minutes = 15;
static int g_publish_enabled = 1;
static int g_publish_human = 1;
static int g_publish_vehicle = 1;
static int g_publish_unknown = 0;
static time_t g_next_publish_time = 0;
static pthread_mutex_t g_publish_mutex = PTHREAD_MUTEX_INITIALIZER;

// Radar occupancy settings and runtime state
typedef struct {
	char id[32];
	char class_name[32];
	int class_bucket;
	int confidence;
	double x;
	double y;
	double speed;
	time_t first_seen;
	time_t last_seen;
	int active;
	int valid;
	int inside_area;
	int counted_inside;
	int counted_bucket;
} RadarObjectState;

static RadarObjectState g_radar_objects[RADAR_MAX_OBJECTS];
static int g_radar_object_count = 0;
static int g_radar_mode = RADAR_MODE_MAXIMUM;
static int g_radar_stale_timeout_seconds = 600;
static int g_radar_min_dwell_seconds = 0;
static int g_radar_min_confidence = 30;
static int g_radar_object_class_bucket = 1;
static int g_radar_aoi_enabled = 0;
static int g_radar_aoi_x1 = 0;
static int g_radar_aoi_x2 = 1000;
static int g_radar_aoi_y1 = 0;
static int g_radar_aoi_y2 = 1000;
static double g_radar_area_x[RADAR_MAX_POLYGON_POINTS] = {0, 1000, 1000, 0};
static double g_radar_area_y[RADAR_MAX_POLYGON_POINTS] = {0, 0, 1000, 1000};
static int g_radar_area_point_count = 4;
static int g_radar_current_total = 0;
static int g_radar_current_human = 0;
static int g_radar_current_vehicle = 0;
static int g_radar_current_unknown = 0;
static int g_radar_peak_total = 0;
static int g_radar_peak_human = 0;
static int g_radar_peak_vehicle = 0;
static int g_radar_peak_unknown = 0;
static int g_radar_connected = 0;
static time_t g_radar_last_frame_time = 0;
static time_t g_radar_last_alert_time = 0;
static int g_radar_alert_active = 0;
static int g_radar_alert_pending = 0;
static pthread_mutex_t g_radar_mutex = PTHREAD_MUTEX_INITIALIZER;

// Settings cache
static char g_b100_ip[64] = "192.168.0.3";
static int g_b100_port = 80;
static char g_callback_ip[64] = "192.168.0.2";
static int g_callback_port = 80;
static char g_callback_digest_user[32] = "aib100";
static char g_callback_digest_password[32] = "aib100";
static int g_lorawan_port = 10;
static int g_health_check_interval = 60;

// App start time for uptime calculation
static time_t g_app_start_time = 0;

// Connection health-check state (all protected by g_health_mutex)
// Every 10th uplink is sent confirmed to validate the link.
// If no ACK within 4 minutes: send "Hello" on port 7 (up to 3 times).
// After 3 failures: force a rejoin.
static int g_unconf_count = 0;        // counts consecutive unconfirmed uplinks
static int g_awaiting_confirm = 0;    // 1 while waiting for a confirmed ACK
static int g_conf_trial_count = 0;    // number of Hello-probe retries attempted
static time_t g_conf_sent_time = 0;   // when the last confirmed uplink was sent
static pthread_mutex_t g_health_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================================================================
// Radar Occupancy Management
// ==================================================================

static int
Clamp_Int(int value, int min_value, int max_value) {
	if (value < min_value) return min_value;
	if (value > max_value) return max_value;
	return value;
}

static const char*
Radar_Mode_Name(int mode) {
	switch (mode) {
		case RADAR_MODE_ALERT: return "alert";
		case RADAR_MODE_ENTRY_EXIT: return "entry_exit";
		case RADAR_MODE_MAXIMUM:
		default: return "maximum";
	}
}

static int
Radar_Mode_From_String(const char* value) {
	if (!value) return RADAR_MODE_MAXIMUM;
	if (strcasecmp(value, "alert") == 0) return RADAR_MODE_ALERT;
	if (strcasecmp(value, "entry_exit") == 0) return RADAR_MODE_ENTRY_EXIT;
	if (strcasecmp(value, "entryExit") == 0) return RADAR_MODE_ENTRY_EXIT;
	return RADAR_MODE_MAXIMUM;
}

static const char*
Radar_Selected_Class_Name(void) {
	return g_radar_object_class_bucket == 2 ? "Vehicle" : "Human";
}

static int
Radar_Class_From_String(const char* value) {
	if (value && strcasecmp(value, "vehicle") == 0) return 2;
	return 1;
}

static const char*
Radar_Mode_Display_Name(int mode) {
	switch (mode) {
		case RADAR_MODE_ALERT: return "Presence alert";
		case RADAR_MODE_ENTRY_EXIT: return "Area balance";
		case RADAR_MODE_MAXIMUM:
		default: return "Interval peak";
	}
}

static int
Radar_Class_Bucket(const char* class_name) {
	if (!class_name || !class_name[0]) return g_radar_object_class_bucket;
	if (strcasecmp(class_name, "Human") == 0) return 1;
	if (strcasecmp(class_name, "Vehicle") == 0) return 2;
	return g_radar_object_class_bucket;
}

static int
Radar_Class_Enabled(int bucket) {
	return bucket == g_radar_object_class_bucket;
}

static void
Radar_Set_Rectangle_Area_Points(void) {
	g_radar_area_point_count = 4;
	g_radar_area_x[0] = g_radar_aoi_x1;
	g_radar_area_y[0] = g_radar_aoi_y1;
	g_radar_area_x[1] = g_radar_aoi_x2;
	g_radar_area_y[1] = g_radar_aoi_y1;
	g_radar_area_x[2] = g_radar_aoi_x2;
	g_radar_area_y[2] = g_radar_aoi_y2;
	g_radar_area_x[3] = g_radar_aoi_x1;
	g_radar_area_y[3] = g_radar_aoi_y2;
}

static void
Radar_Load_Area_Points(cJSON* points) {
	if (!points || !cJSON_IsArray(points) || cJSON_GetArraySize(points) < 3) {
		Radar_Set_Rectangle_Area_Points();
		return;
	}

	int count = 0;
	double min_x = 1000, max_x = 0, min_y = 1000, max_y = 0;
	cJSON* point = NULL;
	cJSON_ArrayForEach(point, points) {
		if (count >= RADAR_MAX_POLYGON_POINTS)
			break;

		cJSON* x_json = cJSON_GetObjectItem(point, "x");
		cJSON* y_json = cJSON_GetObjectItem(point, "y");
		if (!x_json || !y_json)
			continue;

		double x = Clamp_Int(x_json->valueint, 0, 1000);
		double y = Clamp_Int(y_json->valueint, 0, 1000);
		g_radar_area_x[count] = x;
		g_radar_area_y[count] = y;
		if (x < min_x) min_x = x;
		if (x > max_x) max_x = x;
		if (y < min_y) min_y = y;
		if (y > max_y) max_y = y;
		count++;
	}

	if (count < 3) {
		Radar_Set_Rectangle_Area_Points();
		return;
	}

	g_radar_area_point_count = count;
	g_radar_aoi_x1 = (int)min_x;
	g_radar_aoi_x2 = (int)max_x;
	g_radar_aoi_y1 = (int)min_y;
	g_radar_aoi_y2 = (int)max_y;
}

static int
Radar_Point_In_Area(double x, double y) {
	if (!g_radar_aoi_enabled)
		return 1;

	if (g_radar_area_point_count < 3)
		return (x >= g_radar_aoi_x1 && x <= g_radar_aoi_x2 && y >= g_radar_aoi_y1 && y <= g_radar_aoi_y2);

	int inside = 0;
	for (int i = 0, j = g_radar_area_point_count - 1; i < g_radar_area_point_count; j = i++) {
		double xi = g_radar_area_x[i], yi = g_radar_area_y[i];
		double xj = g_radar_area_x[j], yj = g_radar_area_y[j];
		if (((yi > y) != (yj > y)) &&
		    (x < (xj - xi) * (y - yi) / ((yj - yi) == 0 ? 0.000001 : (yj - yi)) + xi))
			inside = !inside;
	}
	return inside;
}

static void
Radar_Update_Peak_From_Current(void) {
	if (g_radar_current_total > g_radar_peak_total) {
		g_radar_peak_total = g_radar_current_total;
		g_radar_peak_human = g_radar_current_human;
		g_radar_peak_vehicle = g_radar_current_vehicle;
		g_radar_peak_unknown = g_radar_current_unknown;
	}
}

static void
Radar_Set_Current_Counts(int total, int human, int vehicle, int unknown) {
	g_radar_current_total = total;
	g_radar_current_human = human;
	g_radar_current_vehicle = vehicle;
	g_radar_current_unknown = unknown;
	Radar_Update_Peak_From_Current();
}

static void
Radar_Adjust_Entry_Exit_Count(int bucket, int delta) {
	g_radar_current_total += delta;
	if (bucket == 1) g_radar_current_human += delta;
	else if (bucket == 2) g_radar_current_vehicle += delta;
	else g_radar_current_unknown += delta;

	if (g_radar_current_total < 0) g_radar_current_total = 0;
	if (g_radar_current_human < 0) g_radar_current_human = 0;
	if (g_radar_current_vehicle < 0) g_radar_current_vehicle = 0;
	if (g_radar_current_unknown < 0) g_radar_current_unknown = 0;
	Radar_Update_Peak_From_Current();
}

static RadarObjectState*
Radar_Find_Or_Create_Object(const char* id) {
	for (int i = 0; i < g_radar_object_count; i++) {
		if (strcmp(g_radar_objects[i].id, id) == 0)
			return &g_radar_objects[i];
	}

	if (g_radar_object_count >= RADAR_MAX_OBJECTS)
		return NULL;

	RadarObjectState* object = &g_radar_objects[g_radar_object_count++];
	memset(object, 0, sizeof(RadarObjectState));
	strncpy(object->id, id, sizeof(object->id) - 1);
	return object;
}

static int
Radar_Object_Is_Countable(const RadarObjectState* object, time_t now) {
	if (!object || !object->valid) return 0;
	if (!Radar_Class_Enabled(object->class_bucket)) return 0;
	if ((now - object->first_seen) < g_radar_min_dwell_seconds) return 0;
	return object->active;
}

static void
Radar_Prune_Expired(time_t now) {
	for (int i = 0; i < g_radar_object_count; ) {
		int remove_object = (now - g_radar_objects[i].last_seen) > 60;

		if (remove_object) {
			if (i < g_radar_object_count - 1)
				memmove(&g_radar_objects[i], &g_radar_objects[i + 1], (g_radar_object_count - i - 1) * sizeof(RadarObjectState));
			g_radar_object_count--;
			memset(&g_radar_objects[g_radar_object_count], 0, sizeof(RadarObjectState));
			continue;
		}
		i++;
	}
}

static void
Radar_Current_Frame_Counts(time_t now, int* total, int* human, int* vehicle, int* unknown) {
	*total = *human = *vehicle = *unknown = 0;
	for (int i = 0; i < g_radar_object_count; i++) {
		RadarObjectState* object = &g_radar_objects[i];
		if (!Radar_Object_Is_Countable(object, now))
			continue;

		(*total)++;
		if (object->class_bucket == 1) (*human)++;
		else if (object->class_bucket == 2) (*vehicle)++;
		else (*unknown)++;
	}
}

static void
Radar_Published_Counts(int* total, int* human, int* vehicle, int* unknown) {
	if (g_radar_mode == RADAR_MODE_ALERT) {
		*total = g_radar_current_total;
		*human = g_radar_current_human;
		*vehicle = g_radar_current_vehicle;
		*unknown = 0;
		return;
	}
	*total = g_radar_peak_total;
	*human = g_radar_peak_human;
	*vehicle = g_radar_peak_vehicle;
	*unknown = 0;
}

static void
Radar_Reset_Interval_State(void) {
	g_radar_peak_total = 0;
	g_radar_peak_human = 0;
	g_radar_peak_vehicle = 0;
	g_radar_peak_unknown = 0;

	if (g_radar_mode == RADAR_MODE_ENTRY_EXIT) {
		g_radar_current_total = 0;
		g_radar_current_human = 0;
		g_radar_current_vehicle = 0;
		g_radar_current_unknown = 0;
		for (int i = 0; i < g_radar_object_count; i++) {
			g_radar_objects[i].counted_inside = 0;
			g_radar_objects[i].counted_bucket = 0;
		}
	} else {
		Radar_Set_Current_Counts(0, 0, 0, 0);
	}
	g_radar_alert_pending = 0;
	g_radar_alert_active = 0;
}

static void
Radar_Apply_Config_To_Subscriber(void) {
	cJSON* config = cJSON_CreateObject();
	if (!config) return;
	cJSON_AddNumberToObject(config, "confidence", g_radar_min_confidence);
	cJSON_AddNumberToObject(config, "units", 1);
	cJSON_AddItemToObject(config, "ignoreClass", cJSON_CreateArray());
	cJSON* aoi = cJSON_CreateObject();
	cJSON_AddNumberToObject(aoi, "x1", g_radar_aoi_enabled ? g_radar_aoi_x1 : 0);
	cJSON_AddNumberToObject(aoi, "x2", g_radar_aoi_enabled ? g_radar_aoi_x2 : 1000);
	cJSON_AddNumberToObject(aoi, "y1", g_radar_aoi_enabled ? g_radar_aoi_y1 : 0);
	cJSON_AddNumberToObject(aoi, "y2", g_radar_aoi_enabled ? g_radar_aoi_y2 : 1000);
	cJSON_AddItemToObject(config, "aoi", aoi);
	RadarDetection_Config(config);
	cJSON_Delete(config);
}

static void
Radar_Detections_Callback(cJSON* detections) {
	time_t now = time(NULL);
	pthread_mutex_lock(&g_radar_mutex);
	g_radar_connected = 1;
	g_radar_last_frame_time = now;

	for (int i = 0; i < g_radar_object_count; i++)
		g_radar_objects[i].active = 0;

	cJSON* item = NULL;
	cJSON_ArrayForEach(item, detections) {
		cJSON* id_json = cJSON_GetObjectItem(item, "id");
		if (!id_json || !id_json->valuestring) continue;

		RadarObjectState* object = Radar_Find_Or_Create_Object(id_json->valuestring);
		if (!object) continue;

		cJSON* class_json = cJSON_GetObjectItem(item, "class");
		const char* detected_class_name = class_json && class_json->valuestring ? class_json->valuestring : "Unknown";
		const char* class_name = detected_class_name;
		int bucket = Radar_Class_Bucket(class_name);
		if (strcasecmp(detected_class_name, "Human") != 0 && strcasecmp(detected_class_name, "Vehicle") != 0)
			class_name = Radar_Selected_Class_Name();
		cJSON* confidence_json = cJSON_GetObjectItem(item, "confidence");
		int confidence = confidence_json ? confidence_json->valueint : 100;
		cJSON* x_json = cJSON_GetObjectItem(item, "cx");
		cJSON* y_json = cJSON_GetObjectItem(item, "cy");
		double x = x_json ? x_json->valuedouble : 0;
		double y = y_json ? y_json->valuedouble : 0;

		int in_aoi = Radar_Point_In_Area(x, y);
		int valid = confidence >= g_radar_min_confidence && in_aoi && Radar_Class_Enabled(bucket);
		int dwell_ok = object->first_seen != 0 && (now - object->first_seen) >= g_radar_min_dwell_seconds;
		int was_inside = object->inside_area;

		if (object->first_seen == 0)
			object->first_seen = now;
		object->last_seen = now;
		object->active = 1;
		object->valid = valid;
		object->class_bucket = bucket;
		object->confidence = confidence;
		object->x = x;
		object->y = y;
		object->inside_area = valid;
		strncpy(object->class_name, class_name, sizeof(object->class_name) - 1);
		object->class_name[sizeof(object->class_name) - 1] = '\0';

		if (g_radar_mode == RADAR_MODE_ENTRY_EXIT && Radar_Class_Enabled(bucket) && confidence >= g_radar_min_confidence && dwell_ok) {
			if (in_aoi && !object->counted_inside) {
				Radar_Adjust_Entry_Exit_Count(bucket, 1);
				object->counted_inside = 1;
				object->counted_bucket = bucket;
			} else if (was_inside && !in_aoi && object->counted_inside) {
				Radar_Adjust_Entry_Exit_Count(object->counted_bucket ? object->counted_bucket : bucket, -1);
				object->counted_inside = 0;
				object->counted_bucket = 0;
			}
		}

		cJSON* radar = cJSON_GetObjectItem(item, "radar");
		cJSON* speed_json = radar ? cJSON_GetObjectItem(radar, "speed") : cJSON_GetObjectItem(item, "speed");
		if (speed_json) object->speed = speed_json->valuedouble;
	}

	Radar_Prune_Expired(now);
	int total, human, vehicle, unknown;
	if (g_radar_mode == RADAR_MODE_MAXIMUM || g_radar_mode == RADAR_MODE_ALERT) {
		Radar_Current_Frame_Counts(now, &total, &human, &vehicle, &unknown);
		Radar_Set_Current_Counts(total, human, vehicle, unknown);
		if (g_radar_mode == RADAR_MODE_ALERT) {
			if (total > 0 && !g_radar_alert_active) {
				g_radar_alert_pending = 1;
				g_radar_alert_active = 1;
			} else if (total == 0) {
				g_radar_alert_active = 0;
			}
		}
	} else {
		total = g_radar_current_total;
		human = g_radar_current_human;
		vehicle = g_radar_current_vehicle;
		unknown = g_radar_current_unknown;
	}
	ACAP_STATUS_SetBool("radar", "connected", 1);
	ACAP_STATUS_SetNumber("radar", "occupancy", total);
	ACAP_STATUS_SetNumber("radar", "human", human);
	ACAP_STATUS_SetNumber("radar", "vehicle", vehicle);
	ACAP_STATUS_SetNumber("radar", "unknown", unknown);
	ACAP_STATUS_SetNumber("radar", "trackedObjects", g_radar_object_count);
	ACAP_STATUS_SetString("radar", "mode", Radar_Mode_Name(g_radar_mode));
	ACAP_STATUS_SetString("radar", "modeLabel", Radar_Mode_Display_Name(g_radar_mode));
	ACAP_STATUS_SetNumber("radar", "peakOccupancy", g_radar_peak_total);
	pthread_mutex_unlock(&g_radar_mutex);

	if (detections)
		cJSON_Delete(detections);
}

static void
Radar_Tracker_Callback(cJSON* detection, int timer) {
	if (detection)
		cJSON_Delete(detection);
}

static cJSON*
Radar_Status_JSON(void) {
	pthread_mutex_lock(&g_radar_mutex);
	int total, human, vehicle, unknown;
	time_t now = time(NULL);
	Radar_Prune_Expired(now);
	total = g_radar_current_total;
	human = g_radar_current_human;
	vehicle = g_radar_current_vehicle;
	unknown = g_radar_current_unknown;

	cJSON* root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "connected", g_radar_connected);
	cJSON_AddStringToObject(root, "occupancyMode", Radar_Mode_Name(g_radar_mode));
	cJSON_AddStringToObject(root, "occupancyModeLabel", Radar_Mode_Display_Name(g_radar_mode));
	cJSON_AddStringToObject(root, "objectClass", Radar_Selected_Class_Name());
	cJSON_AddNumberToObject(root, "staleTimeoutSeconds", g_radar_stale_timeout_seconds);
	cJSON_AddNumberToObject(root, "minDwellSeconds", g_radar_min_dwell_seconds);
	cJSON_AddNumberToObject(root, "minConfidence", g_radar_min_confidence);
	cJSON_AddNumberToObject(root, "lastFrameAgeSeconds", g_radar_last_frame_time ? (int)(now - g_radar_last_frame_time) : -1);
	cJSON_AddNumberToObject(root, "lastAlertTime", (double)g_radar_last_alert_time);
	cJSON_AddNumberToObject(root, "trackedObjects", g_radar_object_count);
	cJSON* aoi = cJSON_CreateObject();
	cJSON_AddBoolToObject(aoi, "enabled", g_radar_aoi_enabled);
	cJSON_AddNumberToObject(aoi, "x1", g_radar_aoi_x1);
	cJSON_AddNumberToObject(aoi, "x2", g_radar_aoi_x2);
	cJSON_AddNumberToObject(aoi, "y1", g_radar_aoi_y1);
	cJSON_AddNumberToObject(aoi, "y2", g_radar_aoi_y2);
	cJSON* points = cJSON_CreateArray();
	for (int i = 0; i < g_radar_area_point_count; i++) {
		cJSON* point = cJSON_CreateObject();
		cJSON_AddNumberToObject(point, "x", g_radar_area_x[i]);
		cJSON_AddNumberToObject(point, "y", g_radar_area_y[i]);
		cJSON_AddItemToArray(points, point);
	}
	cJSON_AddItemToObject(aoi, "points", points);
	cJSON_AddItemToObject(root, "aoi", aoi);

	cJSON* counts = cJSON_CreateObject();
	cJSON_AddNumberToObject(counts, "total", total);
	cJSON_AddNumberToObject(counts, "human", human);
	cJSON_AddNumberToObject(counts, "vehicle", vehicle);
	cJSON_AddNumberToObject(counts, "unknown", unknown);
	cJSON_AddItemToObject(root, "occupancy", counts);
	cJSON* peak = cJSON_CreateObject();
	cJSON_AddNumberToObject(peak, "total", g_radar_peak_total);
	cJSON_AddNumberToObject(peak, "human", g_radar_peak_human);
	cJSON_AddNumberToObject(peak, "vehicle", g_radar_peak_vehicle);
	cJSON_AddNumberToObject(peak, "unknown", g_radar_peak_unknown);
	cJSON_AddItemToObject(root, "peakOccupancy", peak);

	cJSON* objects = cJSON_CreateArray();
	for (int i = 0; i < g_radar_object_count; i++) {
		RadarObjectState* object = &g_radar_objects[i];
		cJSON* item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "id", object->id);
		cJSON_AddStringToObject(item, "class", object->class_name[0] ? object->class_name : "Unknown");
		cJSON_AddNumberToObject(item, "confidence", object->confidence);
		cJSON_AddNumberToObject(item, "x", object->x);
		cJSON_AddNumberToObject(item, "y", object->y);
		cJSON_AddNumberToObject(item, "speed", object->speed);
		cJSON_AddNumberToObject(item, "ageSeconds", object->first_seen ? (int)(now - object->first_seen) : 0);
		cJSON_AddNumberToObject(item, "lastSeenSeconds", object->last_seen ? (int)(now - object->last_seen) : -1);
		cJSON_AddBoolToObject(item, "active", object->active);
		cJSON_AddBoolToObject(item, "valid", Radar_Object_Is_Countable(object, now));
		cJSON_AddBoolToObject(item, "insideArea", object->inside_area);
		cJSON_AddItemToArray(objects, item);
	}
	cJSON_AddItemToObject(root, "objects", objects);

	pthread_mutex_lock(&g_publish_mutex);
	cJSON_AddBoolToObject(root, "publishEnabled", g_publish_enabled);
	cJSON_AddNumberToObject(root, "publishInterval", g_publish_interval_minutes);
	cJSON_AddNumberToObject(root, "nextPublishTime", (double)g_next_publish_time);
	int seconds_until_publish = g_next_publish_time > now ? (int)(g_next_publish_time - now) : 0;
	cJSON_AddNumberToObject(root, "secondsUntilPublish", seconds_until_publish);
	pthread_mutex_unlock(&g_publish_mutex);

	pthread_mutex_unlock(&g_radar_mutex);
	return root;
}

static void
Radar_Reset_State(void) {
	pthread_mutex_lock(&g_radar_mutex);
	memset(g_radar_objects, 0, sizeof(g_radar_objects));
	g_radar_object_count = 0;
	g_radar_current_total = 0;
	g_radar_current_human = 0;
	g_radar_current_vehicle = 0;
	g_radar_current_unknown = 0;
	g_radar_peak_total = 0;
	g_radar_peak_human = 0;
	g_radar_peak_vehicle = 0;
	g_radar_peak_unknown = 0;
	pthread_mutex_unlock(&g_radar_mutex);
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

	if (strcmp(service, "lorawan") == 0) {
		cJSON* port = cJSON_GetObjectItem(data, "port");
		if (port) {
			g_lorawan_port = port->valueint;
			if (g_lorawan_port < 1) g_lorawan_port = 1;
			if (g_lorawan_port > 223) g_lorawan_port = 223;
			LOG("Settings: LoRaWAN port updated to %d\n", g_lorawan_port);
		}
	}

	if (strcmp(service, "transmission") == 0) {
		cJSON* interval = cJSON_GetObjectItem(data, "intervalMinutes");
		if (interval) {
			pthread_mutex_lock(&g_publish_mutex);
			g_publish_interval_minutes = interval->valueint;
			g_publish_interval_minutes = Clamp_Int(g_publish_interval_minutes, 5, 60);
			pthread_mutex_unlock(&g_publish_mutex);
		}
		cJSON* enabled = cJSON_GetObjectItem(data, "enabled");
		if (enabled) {
			pthread_mutex_lock(&g_publish_mutex);
			g_publish_enabled = cJSON_IsTrue(enabled);
			pthread_mutex_unlock(&g_publish_mutex);
		}
		cJSON* classes = cJSON_GetObjectItem(data, "classes");
		if (classes) {
			pthread_mutex_lock(&g_publish_mutex);
			cJSON* human = cJSON_GetObjectItem(classes, "human");
			if (human) g_publish_human = cJSON_IsTrue(human);
			cJSON* vehicle = cJSON_GetObjectItem(classes, "vehicle");
			if (vehicle) g_publish_vehicle = cJSON_IsTrue(vehicle);
			cJSON* unknown = cJSON_GetObjectItem(classes, "unknown");
			if (unknown) g_publish_unknown = cJSON_IsTrue(unknown);
			pthread_mutex_unlock(&g_publish_mutex);
		}
	}

	if (strcmp(service, "radar") == 0) {
		pthread_mutex_lock(&g_radar_mutex);
		cJSON* mode = cJSON_GetObjectItem(data, "occupancyMode");
		if (mode && mode->valuestring) g_radar_mode = Radar_Mode_From_String(mode->valuestring);
		cJSON* object_class = cJSON_GetObjectItem(data, "objectClass");
		if (object_class && object_class->valuestring) g_radar_object_class_bucket = Radar_Class_From_String(object_class->valuestring);
		cJSON* stale = cJSON_GetObjectItem(data, "staleTimeoutSeconds");
		if (stale) g_radar_stale_timeout_seconds = Clamp_Int(stale->valueint, 30, 3600);
		cJSON* dwell = cJSON_GetObjectItem(data, "minDwellSeconds");
		if (dwell) g_radar_min_dwell_seconds = Clamp_Int(dwell->valueint, 0, 300);
		cJSON* confidence = cJSON_GetObjectItem(data, "minConfidence");
		if (confidence) g_radar_min_confidence = Clamp_Int(confidence->valueint, 0, 100);
		cJSON* aoi = cJSON_GetObjectItem(data, "aoi");
		if (aoi) {
			cJSON* enabled = cJSON_GetObjectItem(aoi, "enabled");
			if (enabled) g_radar_aoi_enabled = cJSON_IsTrue(enabled);
			cJSON* x1 = cJSON_GetObjectItem(aoi, "x1");
			cJSON* x2 = cJSON_GetObjectItem(aoi, "x2");
			cJSON* y1 = cJSON_GetObjectItem(aoi, "y1");
			cJSON* y2 = cJSON_GetObjectItem(aoi, "y2");
			if (x1) g_radar_aoi_x1 = Clamp_Int(x1->valueint, 0, 1000);
			if (x2) g_radar_aoi_x2 = Clamp_Int(x2->valueint, 0, 1000);
			if (y1) g_radar_aoi_y1 = Clamp_Int(y1->valueint, 0, 1000);
			if (y2) g_radar_aoi_y2 = Clamp_Int(y2->valueint, 0, 1000);
			if (g_radar_aoi_x2 < g_radar_aoi_x1) g_radar_aoi_x2 = g_radar_aoi_x1;
			if (g_radar_aoi_y2 < g_radar_aoi_y1) g_radar_aoi_y2 = g_radar_aoi_y1;
			cJSON* points = cJSON_GetObjectItem(aoi, "points");
			Radar_Load_Area_Points(points);
		} else {
			Radar_Set_Rectangle_Area_Points();
		}
		Radar_Reset_Interval_State();
		pthread_mutex_unlock(&g_radar_mutex);
		Radar_Apply_Config_To_Subscriber();
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
static int Publish_Radar_To_LoRa(void);

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
				LOG("Downlink: Reset radar occupancy state\n");
				Radar_Reset_State();
				sleep(1);
				Publish_Radar_To_LoRa();
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
					LOG("Downlink: Set publish interval to %d minutes\n", minutes);
					pthread_mutex_lock(&g_publish_mutex);
					g_publish_interval_minutes = minutes;
					pthread_mutex_unlock(&g_publish_mutex);
					if (settings_obj) {
						cJSON* trans = cJSON_GetObjectItem(settings_obj, "transmission");
						if (trans) {
							cJSON* iv = cJSON_GetObjectItem(trans, "intervalMinutes");
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
				// Signal quality snapshot → reply on port 7
				B100_Status* s = B100_Get_Status();
				char info[128] = {0};
				snprintf(info, sizeof(info), "DR%d,%dB,%.0fdBm,%.1fdB,%uup,%udn",
				         s->dataRate, s->maxPayload,
				         s->rssi, s->snr,
				         s->fcntUp, s->fcntDown);
				LOG("Downlink: Signal Quality reply: %s\n", info);
				if (!B100_Send(info, 7, 0))
					LOG_WARN("Downlink: Signal Quality send failed: %s\n", B100_Get_Last_Error());
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

		// If callbacks are not yet configured, or if B100 reports http_api_enable=0
		// (e.g. after a B100 restart or external reconfiguration), reapply.
		if (!g_callbacks_configured || !status->httpApiEnabled) {
			if (g_callbacks_configured && !status->httpApiEnabled)
				LOG_WARN("B100 http_api_enable is 0 — reapplying callback configuration\n");
			g_callbacks_configured = 0;
			const char* cam_ip = g_callback_ip[0] ? g_callback_ip : ACAP_DEVICE_Prop("IPv4");
			char status_uri[64], receive_uri[64];
			snprintf(status_uri, sizeof(status_uri), "/local/%s/%s", APP_PACKAGE, B100_STATUS_CALLBACK_NODE);
			snprintf(receive_uri, sizeof(receive_uri), "/local/%s/%s", APP_PACKAGE, B100_RECEIVE_CALLBACK_NODE);
			if (B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri,
			                           g_callback_digest_user, g_callback_digest_password)) {
				g_callbacks_configured = 1;
				// Also re-apply GPS callback so GPS push stays active
				char gps_uri[64];
				snprintf(gps_uri, sizeof(gps_uri), "/local/%s/%s", APP_PACKAGE, B100_GPS_CALLBACK_NODE);
				B100_Configure_GPS_Callback(gps_uri, 60);
				ACAP_STATUS_SetString("app", "status", "Running");
			} else {
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

		// Confirmation timeout: if a confirmed uplink got no ACK within 4 minutes,
		// probe with "Hello" on port 7 (up to 3 retries), then force a rejoin.
		pthread_mutex_lock(&g_health_mutex);
		int waiting = g_awaiting_confirm;
		int trials  = g_conf_trial_count;
		time_t sent = g_conf_sent_time;
		pthread_mutex_unlock(&g_health_mutex);

		if (waiting && (time(NULL) - sent) > 240) {
			if (trials < 3) {
				LOG("Confirmation timeout - sending Hello probe %d/3 on port 7\n", trials + 1);
				B100_Send("Hello", 7, 1);
				pthread_mutex_lock(&g_health_mutex);
				g_conf_trial_count++;
				g_conf_sent_time = time(NULL);
				pthread_mutex_unlock(&g_health_mutex);
			} else {
				LOG("3 confirmation failures - forcing rejoin\n");
				ACAP_STATUS_SetString("lorawan", "statusText", "Link lost - rejoining");
				B100_Join_Auto();
				pthread_mutex_lock(&g_health_mutex);
				g_awaiting_confirm = 0;
				g_conf_trial_count = 0;
				g_unconf_count = 0;
				pthread_mutex_unlock(&g_health_mutex);
			}
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
HTTP_Endpoint_radar(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	cJSON* root = Radar_Status_JSON();
	ACAP_HTTP_Respond_JSON(response, root);
	cJSON_Delete(root);
}

void
HTTP_Endpoint_counters(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	HTTP_Endpoint_radar(response, request);
}

// ==================================================================
// LoRa Publishing — sends binary radar occupancy data
// ==================================================================

int Publish_Radar_To_LoRa() {
	int total, human, vehicle, unknown;
	pthread_mutex_lock(&g_radar_mutex);
	Radar_Published_Counts(&total, &human, &vehicle, &unknown);
	int mode = g_radar_mode;
	if (mode == RADAR_MODE_ALERT && (!g_radar_alert_pending || total <= 0)) {
		if (total <= 0)
			g_radar_alert_pending = 0;
		pthread_mutex_unlock(&g_radar_mutex);
		LOG("LoRa: Alert mode skipped publish, no alert pending\n");
		return -1;
	}
	pthread_mutex_unlock(&g_radar_mutex);

	unsigned char buffer[10];
	buffer[0] = 1;
	buffer[1] = (unsigned char)mode;
	uint16_t values[4] = {
		(uint16_t)(total & 0xFFFF),
		(uint16_t)(human & 0xFFFF),
		(uint16_t)(vehicle & 0xFFFF),
		(uint16_t)(unknown & 0xFFFF)
	};
	for (int i = 0; i < 4; i++) {
		buffer[2 + (i * 2)] = values[i] & 0xFF;
		buffer[3 + (i * 2)] = (values[i] >> 8) & 0xFF;
	}
	
	// Decide confirmed/unconfirmed: every 10th uplink is confirmed for link health.
	pthread_mutex_lock(&g_health_mutex);
	g_unconf_count++;
	int use_confirmed = (g_unconf_count >= 10) ? 1 : 0;
	if (use_confirmed) {
		g_unconf_count = 0;
		g_awaiting_confirm = 1;
		g_conf_trial_count = 0;
		g_conf_sent_time = time(NULL);
		LOG("LoRa: Sending health-check confirmed uplink\n");
	}
	pthread_mutex_unlock(&g_health_mutex);

	// Send raw bytes directly via the B100 POST API (no base64 needed)
	LOG("LoRa: Publishing radar occupancy total=%d human=%d vehicle=%d unknown=%d on port %d%s\n",
	    total, human, vehicle, unknown, g_lorawan_port,
	    use_confirmed ? " [confirmed]" : "");

	int success = B100_Send_Bytes(buffer, (int)sizeof(buffer), g_lorawan_port, use_confirmed);
	
	if (success) {
		LOG("LoRa: Publish accepted\n");
		pthread_mutex_lock(&g_radar_mutex);
		if (g_radar_mode == RADAR_MODE_ALERT) {
			g_radar_alert_pending = 0;
			g_radar_last_alert_time = time(NULL);
		} else {
			Radar_Reset_Interval_State();
		}
		pthread_mutex_unlock(&g_radar_mutex);
		pthread_mutex_lock(&g_publish_mutex);
		g_next_publish_time = time(NULL) + (g_publish_interval_minutes * 60);
		pthread_mutex_unlock(&g_publish_mutex);
	} else {
		LOG_WARN("LoRa: Publish failed - %s\n", B100_Get_Last_Error());
	}
	
	return success;
}

void*
Publish_Thread(void* arg) {
	LOG("Publish thread started\n");
	
	// Set initial next publish time
	pthread_mutex_lock(&g_publish_mutex);
	g_next_publish_time = time(NULL) + (g_publish_interval_minutes * 60);
	pthread_mutex_unlock(&g_publish_mutex);
	
	while (running) {
		pthread_mutex_lock(&g_publish_mutex);
		int enabled = g_publish_enabled;
		time_t next_time = g_next_publish_time;
		pthread_mutex_unlock(&g_publish_mutex);
		
		if (enabled) {
			time_t now = time(NULL);
			pthread_mutex_lock(&g_radar_mutex);
			int alert_mode = g_radar_mode == RADAR_MODE_ALERT;
			int alert_pending = g_radar_alert_pending;
			pthread_mutex_unlock(&g_radar_mutex);

			if ((alert_mode && alert_pending) || (!alert_mode && now >= next_time)) {
				// Time to publish
				Publish_Radar_To_LoRa();
			}
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
	
	int result = Publish_Radar_To_LoRa();
	if (result > 0) {
		ACAP_HTTP_Respond_Text(response, "Radar occupancy published");
	} else if (result < 0) {
		ACAP_HTTP_Respond_Error(response, 409, "No alert pending");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to publish radar occupancy");
	}
}

void
HTTP_Endpoint_radar_reset(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "POST") != 0) {
		ACAP_HTTP_Respond_Error(response, 400, "Method must be POST");
		return;
	}
	Radar_Reset_State();
	ACAP_HTTP_Respond_Text(response, "Radar occupancy state reset");
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
	const char* cam_ip = g_callback_ip[0] ? g_callback_ip : ACAP_DEVICE_Prop("IPv4");
	char status_uri[64], receive_uri[64];
	snprintf(status_uri, sizeof(status_uri), "/local/%s/%s", APP_PACKAGE, B100_STATUS_CALLBACK_NODE);
	snprintf(receive_uri, sizeof(receive_uri), "/local/%s/%s", APP_PACKAGE, B100_RECEIVE_CALLBACK_NODE);
	if (!B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri,
	                            g_callback_digest_user, g_callback_digest_password)) {
		ACAP_HTTP_Respond_Error(response, 500, "Failed to configure B100 callbacks");
		return;
	}
	g_callbacks_configured = 1;

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
	cJSON* info = B100_Get_Info();
	if (info) {
		// Add current callback config from the bridge parameter API.
		cJSON* cb_params = B100_Get_Params(NULL);
		if (cb_params) {
			cJSON* addr = cJSON_GetObjectItem(cb_params, "callback_addr");
			if (addr && addr->valuestring)
				cJSON_AddStringToObject(info, "callback_configured_addr", addr->valuestring);
			cJSON* port = cJSON_GetObjectItem(cb_params, "callback_port");
			if (port)
				cJSON_AddNumberToObject(info, "callback_configured_port", port->valueint);
			cJSON* user = cJSON_GetObjectItem(cb_params, "callback_digest_user");
			if (user && user->valuestring)
				cJSON_AddStringToObject(info, "callback_digest_user", user->valuestring);
			cJSON* status_uri = cJSON_GetObjectItem(cb_params, "callback_status_uri");
			if (status_uri && status_uri->valuestring)
				cJSON_AddStringToObject(info, "callback_status_uri", status_uri->valuestring);
			cJSON* receive_uri = cJSON_GetObjectItem(cb_params, "callback_receive_uri");
			if (receive_uri && receive_uri->valuestring)
				cJSON_AddStringToObject(info, "callback_receive_uri", receive_uri->valuestring);
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
	
	if (B100_Link_Check()) {
		ACAP_HTTP_Respond_Text(response, "Linkcheck request sent");
	} else {
		ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	}
}

void
HTTP_Endpoint_translator(const ACAP_HTTP_Response response, const ACAP_HTTP_Request request) {
	const char* dev_model  = ACAP_DEVICE_Prop("model")  ? ACAP_DEVICE_Prop("model")  : "unknown";
	const char* dev_serial = ACAP_DEVICE_Prop("serial") ? ACAP_DEVICE_Prop("serial") : "000000";
	const char* dev_date   = ACAP_DEVICE_Date();
	char js[8192];
	snprintf(js, sizeof(js),
		"/**\n"
		" * AI-B100 Radar Occupancy Decoder\n"
		" * Device  : %s  (serial %s)\n"
		" * Generated: %s\n"
		" *\n"
		" * Payload format\n"
		" * --------------\n"
		" * byte 0     protocol version, currently 1\n"
		" * byte 1     mode: 0=Interval peak, 1=Area balance, 2=Presence alert\n"
		" * bytes 2-3  total occupancy, uint16 little-endian\n"
		" * bytes 4-5  human occupancy, uint16 little-endian\n"
		" * bytes 6-7  vehicle occupancy, uint16 little-endian\n"
		" * bytes 8-9  unknown occupancy, uint16 little-endian, currently always 0\n"
		" *\n"
		" * Mode semantics\n"
		" * --------------\n"
		" * Interval peak:   highest count observed during the publish interval.\n"
		" * Area balance:    current balanced inside-area count from entry/exit transitions.\n"
		" * Presence alert:  current count when a new detection episode starts; no interval publish.\n"
		" *\n"
		" */\n\n"
		"function decodeRadarOccupancy(bytes) {\n"
		"  if (!bytes || bytes.length < 10) {\n"
		"    throw new Error('Radar occupancy payload must be 10 bytes');\n"
		"  }\n"
		"  var modeInfo = {\n"
		"    0: {\n"
		"      name: 'maximum',\n"
		"      label: 'Interval peak',\n"
		"      description: 'Highest count observed during the publish interval.'\n"
		"    },\n"
		"    1: {\n"
		"      name: 'entry_exit',\n"
		"      label: 'Area balance',\n"
		"      description: 'Current balanced inside-area count from entry/exit transitions.'\n"
		"    },\n"
		"    2: {\n"
		"      name: 'alert',\n"
		"      label: 'Presence alert',\n"
		"      description: 'Current count when a new detection episode starts; no interval publish.'\n"
		"    }\n"
		"  };\n"
		"  function u16(offset) { return bytes[offset] | (bytes[offset + 1] << 8); }\n"
		"  var modeCode = bytes[1];\n"
		"  var mode = modeInfo[modeCode] || {\n"
		"    name: 'unknown',\n"
		"    label: 'Unknown',\n"
		"    description: 'Unknown radar occupancy mode.'\n"
		"  };\n"
		"  var occupancy = {\n"
		"    total: u16(2),\n"
		"    human: u16(4),\n"
		"    vehicle: u16(6),\n"
		"    unknown: u16(8)\n"
		"  };\n"
		"  return {\n"
		"    version: bytes[0],\n"
		"    modeCode: modeCode,\n"
		"    mode: mode.name,\n"
		"    modeLabel: mode.label,\n"
		"    modeDescription: mode.description,\n"
		"    total: occupancy.total,\n"
		"    human: occupancy.human,\n"
		"    vehicle: occupancy.vehicle,\n"
		"    unknown: occupancy.unknown,\n"
		"    occupancy: occupancy,\n"
		"    layout: {\n"
		"      byte0: 'protocol version',\n"
		"      byte1: 'mode: 0=Interval peak, 1=Area balance, 2=Presence alert',\n"
		"      bytes2to3: 'total occupancy, uint16 little-endian',\n"
		"      bytes4to5: 'human occupancy, uint16 little-endian',\n"
		"      bytes6to7: 'vehicle occupancy, uint16 little-endian',\n"
		"      bytes8to9: 'unknown occupancy, uint16 little-endian'\n"
		"    }\n"
		"  };\n"
		"}\n\n"
		"function decodeUplink(input) {\n"
		"  try {\n"
		"    return { data: decodeRadarOccupancy(input.bytes) };\n"
		"  } catch (error) {\n"
		"    return { errors: [error.message] };\n"
		"  }\n"
		"}\n",
		dev_model, dev_serial, dev_date
	);

	char filename[128];
	snprintf(filename, sizeof(filename), "radar-occupancy-decoder-%s-%s-%s.js",
	         dev_model, dev_serial, dev_date);

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
    ACAP_Init(APP_PACKAGE, Settings_Updated_Callback);
    
	LOG("Initializing radar scene subscriber...\n");
	int radar_status = RadarDetection_Init(Radar_Detections_Callback, Radar_Tracker_Callback);
	if (radar_status > 0) {
		g_radar_connected = 1;
		ACAP_STATUS_SetString("radar", "status", "Subscribed");
	} else {
		g_radar_connected = 0;
		ACAP_STATUS_SetString("radar", "status", "Radar subscriber failed");
		LOG_WARN("Radar subscriber initialization failed: %d\n", radar_status);
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
            cJSON* port = cJSON_GetObjectItem(lorawan, "port");
            if (port) {
                g_lorawan_port = port->valueint;
            }
            
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
            cJSON* interval = cJSON_GetObjectItem(transmission, "intervalMinutes");
            if (interval) {
                g_publish_interval_minutes = interval->valueint;
				g_publish_interval_minutes = Clamp_Int(g_publish_interval_minutes, 5, 60);
            }
            
            cJSON* enabled = cJSON_GetObjectItem(transmission, "enabled");
            if (enabled) {
                g_publish_enabled = cJSON_IsTrue(enabled);
            }
            
            cJSON* classes = cJSON_GetObjectItem(transmission, "classes");
            if (classes) {
                cJSON* human = cJSON_GetObjectItem(classes, "human");
                if (human) g_publish_human = cJSON_IsTrue(human);
				cJSON* vehicle = cJSON_GetObjectItem(classes, "vehicle");
				if (vehicle) g_publish_vehicle = cJSON_IsTrue(vehicle);
				cJSON* unknown = cJSON_GetObjectItem(classes, "unknown");
				if (unknown) g_publish_unknown = cJSON_IsTrue(unknown);
				if (g_publish_vehicle && !g_publish_human) g_radar_object_class_bucket = 2;
				else if (g_publish_human && !g_publish_vehicle) g_radar_object_class_bucket = 1;
            }
        }

		cJSON* radar = cJSON_GetObjectItem(settings, "radar");
		if (radar) {
			cJSON* mode = cJSON_GetObjectItem(radar, "occupancyMode");
			if (mode && mode->valuestring) g_radar_mode = Radar_Mode_From_String(mode->valuestring);
			cJSON* object_class = cJSON_GetObjectItem(radar, "objectClass");
			if (object_class && object_class->valuestring) g_radar_object_class_bucket = Radar_Class_From_String(object_class->valuestring);
			cJSON* stale = cJSON_GetObjectItem(radar, "staleTimeoutSeconds");
			if (stale) g_radar_stale_timeout_seconds = Clamp_Int(stale->valueint, 30, 3600);
			cJSON* dwell = cJSON_GetObjectItem(radar, "minDwellSeconds");
			if (dwell) g_radar_min_dwell_seconds = Clamp_Int(dwell->valueint, 0, 300);
			cJSON* confidence = cJSON_GetObjectItem(radar, "minConfidence");
			if (confidence) g_radar_min_confidence = Clamp_Int(confidence->valueint, 0, 100);
			cJSON* aoi = cJSON_GetObjectItem(radar, "aoi");
			if (aoi) {
				cJSON* enabled = cJSON_GetObjectItem(aoi, "enabled");
				if (enabled) g_radar_aoi_enabled = cJSON_IsTrue(enabled);
				cJSON* x1 = cJSON_GetObjectItem(aoi, "x1");
				cJSON* x2 = cJSON_GetObjectItem(aoi, "x2");
				cJSON* y1 = cJSON_GetObjectItem(aoi, "y1");
				cJSON* y2 = cJSON_GetObjectItem(aoi, "y2");
				if (x1) g_radar_aoi_x1 = Clamp_Int(x1->valueint, 0, 1000);
				if (x2) g_radar_aoi_x2 = Clamp_Int(x2->valueint, 0, 1000);
				if (y1) g_radar_aoi_y1 = Clamp_Int(y1->valueint, 0, 1000);
				if (y2) g_radar_aoi_y2 = Clamp_Int(y2->valueint, 0, 1000);
				cJSON* points = cJSON_GetObjectItem(aoi, "points");
				Radar_Load_Area_Points(points);
			}
			Radar_Apply_Config_To_Subscriber();
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
			snprintf(status_uri, sizeof(status_uri), "/local/%s/%s", APP_PACKAGE, B100_STATUS_CALLBACK_NODE);
			snprintf(receive_uri, sizeof(receive_uri), "/local/%s/%s", APP_PACKAGE, B100_RECEIVE_CALLBACK_NODE);
			if (B100_Configure_Callbacks(cam_ip, g_callback_port, status_uri, receive_uri,
										 g_callback_digest_user, g_callback_digest_password)) {
                g_callbacks_configured = 1;
                // Configure GPS callback — POST every 60 s to the b100_gps endpoint
                char gps_uri[64];
				snprintf(gps_uri, sizeof(gps_uri), "/local/%s/%s", APP_PACKAGE, B100_GPS_CALLBACK_NODE);
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
	ACAP_HTTP_Node("radar", HTTP_Endpoint_radar);
	ACAP_HTTP_Node("radar_reset", HTTP_Endpoint_radar_reset);
    ACAP_HTTP_Node("publish", HTTP_Endpoint_publish);
    ACAP_HTTP_Node("translator", HTTP_Endpoint_translator);
    ACAP_HTTP_Node("receive", HTTP_Endpoint_receive);
	ACAP_HTTP_Node(B100_STATUS_CALLBACK_NODE, HTTP_Endpoint_B100_Status_Callback);
	ACAP_HTTP_Node(B100_RECEIVE_CALLBACK_NODE, HTTP_Endpoint_B100_Receive_Callback);
	ACAP_HTTP_Node(B100_GPS_CALLBACK_NODE, HTTP_Endpoint_B100_GPS_Callback);
    ACAP_HTTP_Node("gps", HTTP_Endpoint_gps);
    ACAP_HTTP_Node("b100_info", HTTP_Endpoint_b100_info);
    ACAP_HTTP_Node("b100_params", HTTP_Endpoint_b100_params);
    ACAP_HTTP_Node("b100_request_status", HTTP_Endpoint_b100_request_status);
    ACAP_HTTP_Node("linkcheck", HTTP_Endpoint_linkcheck);
    
    // Initialize next publish time
    pthread_mutex_lock(&g_publish_mutex);
    g_next_publish_time = time(NULL) + (g_publish_interval_minutes * 60);
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
	
	RadarDetection_Cleanup();

	B100_Cleanup();
    ACAP_Cleanup();
    
    LOG("Shutdown complete\n");
    closelog();
    return 0;
}
