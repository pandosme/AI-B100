#include <glib.h>
#include <glib-unix.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "ACAP.h"
#include "B100.h"
#include "Model.h"
#include "Video.h"
#include "occupancy.h"
#include "ota_translator.h"

#define APP_PACKAGE "aib100"
#define OCCUPANCY_PORT 2
#define CALLBACK_STATUS "b100_status"
#define CALLBACK_RECEIVE "b100_receive"
#define CALLBACK_GPS "b100_gps"
#define LOG_LIMIT 20

#define LOG(fmt, args...) do { syslog(LOG_INFO, fmt, ##args); printf(fmt, ##args); } while (0)
#define LOG_WARN(fmt, args...) do { syslog(LOG_WARNING, fmt, ##args); printf(fmt, ##args); } while (0)

static GMainLoop* g_main_loop;
static cJSON* g_settings;
static cJSON* g_publish_log;
static cJSON* g_downlink_log;
static pthread_t g_publish_thread;
static pthread_t g_health_thread;
static pthread_mutex_t g_publish_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static cJSON* g_detection_config;
static atomic_bool g_running = true;
static int g_callbacks_configured;
static int g_publish_enabled;
static int g_selection_valid;
static int g_interval_minutes = 15;
static time_t g_next_publish;
static time_t g_last_publish;
static char g_b100_ip[64] = "192.168.1.250";
static int g_b100_port = 81;
static char g_api_user[33] = "lorabridge";
static char g_api_password[65] = "lorabridge";
static char g_callback_ip[64] = "192.168.1.200";
static int g_callback_port = 80;
static char g_callback_user[33] = "lorabridge";
static char g_callback_password[65] = "lorabridge";
static int g_health_interval = 60;
static int g_auto_join = 1;

static int Clamp(int value, int minimum, int maximum) {
	if (value < minimum) return minimum;
	if (value > maximum) return maximum;
	return value;
}

static void Replace_String(cJSON* object, const char* name, const char* value) {
	cJSON* item = cJSON_CreateString(value ? value : "");
	if (cJSON_GetObjectItem(object, name)) cJSON_ReplaceItemInObject(object, name, item);
	else cJSON_AddItemToObject(object, name, item);
}

static void Replace_Number(cJSON* object, const char* name, double value) {
	cJSON* item = cJSON_CreateNumber(value);
	if (cJSON_GetObjectItem(object, name)) cJSON_ReplaceItemInObject(object, name, item);
	else cJSON_AddItemToObject(object, name, item);
}

static void Bytes_To_Hex(const unsigned char* data, size_t length, char* output, size_t capacity) {
	if (!output || capacity == 0) return;
	size_t used = 0;
	for (size_t index = 0; index < length && used + 2 < capacity; index++)
		used += (size_t)snprintf(output + used, capacity - used, "%02X", data[index]);
	output[used] = '\0';
}

static int Hex_Nibble(char value) {
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

static void Append_Publish_Log(const unsigned char* data, size_t length, int success, const char* error) {
	char payload[OCCUPANCY_MAX_LABELS * 2 + 1];
	Bytes_To_Hex(data, length, payload, sizeof(payload));
	cJSON* entry = cJSON_CreateObject();
	cJSON_AddStringToObject(entry, "time", ACAP_DEVICE_Local_Time());
	cJSON_AddStringToObject(entry, "stream", "occupancy");
	cJSON_AddNumberToObject(entry, "port", OCCUPANCY_PORT);
	cJSON_AddNumberToObject(entry, "length", (int)length);
	cJSON_AddStringToObject(entry, "payload", payload);
	cJSON_AddBoolToObject(entry, "success", success);
	if (error && error[0]) cJSON_AddStringToObject(entry, "error", error);
	pthread_mutex_lock(&g_log_mutex);
	if (!g_publish_log) g_publish_log = cJSON_CreateArray();
	cJSON_AddItemToArray(g_publish_log, entry);
	while (cJSON_GetArraySize(g_publish_log) > LOG_LIMIT) cJSON_DeleteItemFromArray(g_publish_log, 0);
	ACAP_STATUS_SetObject("lorawan", "publishes", g_publish_log);
	pthread_mutex_unlock(&g_log_mutex);
}

static void Update_Occupancy_Status(void) {
	OccupancyStatus status;
	Occupancy_Get_Status(&status);
	cJSON* labels = cJSON_CreateArray();
	for (size_t index = 0; index < status.label_count; index++) {
		cJSON* label = cJSON_CreateObject();
		cJSON_AddNumberToObject(label, "byte", (int)index);
		cJSON_AddStringToObject(label, "label", status.labels[index]);
		cJSON_AddNumberToObject(label, "current", status.current[index]);
		cJSON_AddNumberToObject(label, "intervalMaximum", status.maxima[index]);
		cJSON_AddNumberToObject(label, "lastPublished", status.last_published[index]);
		cJSON_AddBoolToObject(label, "clamped", status.maxima[index] > UINT8_MAX);
		cJSON_AddItemToArray(labels, label);
	}
	ACAP_STATUS_SetObject("occupancy", "labels", labels);
	cJSON_Delete(labels);
	pthread_mutex_lock(&g_publish_mutex);
	ACAP_STATUS_SetBool("occupancy", "enabled", g_publish_enabled);
	ACAP_STATUS_SetBool("occupancy", "selectionValid", g_selection_valid);
	ACAP_STATUS_SetBool("occupancy", "publishInFlight", status.publish_in_flight);
	ACAP_STATUS_SetNumber("occupancy", "port", OCCUPANCY_PORT);
	ACAP_STATUS_SetNumber("occupancy", "intervalMinutes", g_interval_minutes);
	ACAP_STATUS_SetNumber("occupancy", "nextPublish", (double)g_next_publish);
	ACAP_STATUS_SetNumber("occupancy", "lastPublish", (double)g_last_publish);
	pthread_mutex_unlock(&g_publish_mutex);
}

static int Configure_Occupancy(cJSON* transmission, int initial) {
	cJSON* config = transmission ? cJSON_GetObjectItem(transmission, "occupancy") : NULL;
	cJSON* selected = config ? cJSON_GetObjectItem(config, "selectedLabels") : NULL;
	const char* labels[OCCUPANCY_MAX_LABELS] = {0};
	size_t label_count = cJSON_IsArray(selected) ? (size_t)cJSON_GetArraySize(selected) : 0;
	int valid = label_count >= 1 && label_count <= OCCUPANCY_MAX_LABELS;
	for (size_t index = 0; valid && index < label_count; index++) {
		cJSON* label = cJSON_GetArrayItem(selected, (int)index);
		if (!cJSON_IsString(label) || !Model_Label_Is_Valid(label->valuestring)) valid = 0;
		else labels[index] = label->valuestring;
		for (size_t previous = 0; valid && previous < index; previous++)
			if (strcmp(labels[index], labels[previous]) == 0) valid = 0;
	}
	int enabled = config && cJSON_IsTrue(cJSON_GetObjectItem(config, "enabled"));
	int interval = Clamp(config && cJSON_IsNumber(cJSON_GetObjectItem(config, "intervalMinutes"))
		? cJSON_GetObjectItem(config, "intervalMinutes")->valueint : 15, 1, 60);
	pthread_mutex_lock(&g_publish_mutex);
	int schedule_changed = (!g_publish_enabled && enabled) || g_interval_minutes != interval;
	g_publish_enabled = enabled;
	g_interval_minutes = interval;
	g_selection_valid = valid;
	if (initial || schedule_changed || g_next_publish == 0) g_next_publish = time(NULL) + interval * 60;
	pthread_mutex_unlock(&g_publish_mutex);
	if (valid) {
		OccupancyStatus old_status;
		Occupancy_Get_Status(&old_status);
		int changed = old_status.label_count != label_count;
		for (size_t index = 0; !changed && index < label_count; index++)
			if (strcmp(old_status.labels[index], labels[index]) != 0) changed = 1;
		if (changed) {
			Occupancy_Configure(labels, label_count);
			pthread_mutex_lock(&g_publish_mutex);
			g_next_publish = time(NULL) + interval * 60;
			pthread_mutex_unlock(&g_publish_mutex);
		}
		ACAP_STATUS_SetNull("occupancy", "error");
	} else {
		ACAP_STATUS_SetString("occupancy", "error", "Select 1-5 unique labels from the active model");
	}
	Update_Occupancy_Status();
	return valid;
}

static void Load_Bridge_Settings(cJSON* b100) {
	if (!b100) return;
	pthread_mutex_lock(&g_runtime_mutex);
	cJSON* item = cJSON_GetObjectItem(b100, "ip");
	if (cJSON_IsString(item)) snprintf(g_b100_ip, sizeof(g_b100_ip), "%s", item->valuestring);
	item = cJSON_GetObjectItem(b100, "port");
	if (cJSON_IsNumber(item)) g_b100_port = Clamp(item->valueint, 1, 65535);
	item = cJSON_GetObjectItem(b100, "apiDigestUser");
	if (cJSON_IsString(item)) snprintf(g_api_user, sizeof(g_api_user), "%s", item->valuestring);
	item = cJSON_GetObjectItem(b100, "apiDigestPassword");
	if (cJSON_IsString(item)) snprintf(g_api_password, sizeof(g_api_password), "%s", item->valuestring);
	item = cJSON_GetObjectItem(b100, "callbackIP");
	if (cJSON_IsString(item)) snprintf(g_callback_ip, sizeof(g_callback_ip), "%s", item->valuestring);
	item = cJSON_GetObjectItem(b100, "callbackPort");
	if (cJSON_IsNumber(item)) g_callback_port = Clamp(item->valueint, 1, 65535);
	item = cJSON_GetObjectItem(b100, "callbackDigestUser");
	if (cJSON_IsString(item)) snprintf(g_callback_user, sizeof(g_callback_user), "%s", item->valuestring);
	item = cJSON_GetObjectItem(b100, "callbackDigestPassword");
	if (cJSON_IsString(item)) snprintf(g_callback_password, sizeof(g_callback_password), "%s", item->valuestring);
	g_callbacks_configured = 0;
	pthread_mutex_unlock(&g_runtime_mutex);
}

static void Settings_Updated(const char* service, cJSON* data) {
	if (!service || !data) return;
	if (strcmp(service, "b100") == 0) {
		Load_Bridge_Settings(data);
		char ip[sizeof(g_b100_ip)], user[sizeof(g_api_user)], password[sizeof(g_api_password)];
		int port;
		pthread_mutex_lock(&g_runtime_mutex);
		snprintf(ip, sizeof(ip), "%s", g_b100_ip);
		snprintf(user, sizeof(user), "%s", g_api_user);
		snprintf(password, sizeof(password), "%s", g_api_password);
		port = g_b100_port;
		pthread_mutex_unlock(&g_runtime_mutex);
		B100_Set_IP(ip);
		B100_Set_Port(port);
		cJSON* timeout = cJSON_GetObjectItem(data, "timeout");
		if (cJSON_IsNumber(timeout)) B100_Set_Timeout(Clamp(timeout->valueint, 25, 30));
		B100_Set_API_Credentials(user, password);
	} else if (strcmp(service, "transmission") == 0) {
		Configure_Occupancy(data, 0);
	} else if (strcmp(service, "detection") == 0) {
		cJSON* copy = cJSON_Duplicate(data, 1);
		pthread_mutex_lock(&g_runtime_mutex);
		cJSON_Delete(g_detection_config);
		g_detection_config = copy;
		pthread_mutex_unlock(&g_runtime_mutex);
		Model_Apply_Detection_Settings(data);
	} else if (strcmp(service, "lorawan") == 0) {
		cJSON* auto_join = cJSON_GetObjectItem(data, "autoJoin");
		pthread_mutex_lock(&g_runtime_mutex);
		g_auto_join = !auto_join || cJSON_IsTrue(auto_join);
		pthread_mutex_unlock(&g_runtime_mutex);
	} else if (strcmp(service, "polling") == 0) {
		cJSON* interval = cJSON_GetObjectItem(data, "healthCheckIntervalSeconds");
		if (cJSON_IsNumber(interval)) {
			pthread_mutex_lock(&g_runtime_mutex);
			g_health_interval = Clamp(interval->valueint, 30, 3600);
			pthread_mutex_unlock(&g_runtime_mutex);
		}
	}
}

static cJSON* Ensure_Object(cJSON* parent, const char* name) {
	cJSON* item = cJSON_GetObjectItem(parent, name);
	if (!cJSON_IsObject(item)) {
		item = cJSON_CreateObject();
		if (cJSON_GetObjectItem(parent, name)) cJSON_ReplaceItemInObject(parent, name, item);
		else cJSON_AddItemToObject(parent, name, item);
	}
	return item;
}

static void Migrate_Settings(void) {
	cJSON* variant = cJSON_GetObjectItem(g_settings, "variant");
	int different_variant = !cJSON_IsString(variant) || strcmp(variant->valuestring, "detectx") != 0;
	Replace_String(g_settings, "variant", "detectx");
	Replace_Number(g_settings, "settingsVersion", 3);
	cJSON* transmission = Ensure_Object(g_settings, "transmission");
	if (different_variant) {
		cJSON* occupancy = cJSON_CreateObject();
		cJSON_AddBoolToObject(occupancy, "enabled", false);
		cJSON_AddNumberToObject(occupancy, "port", OCCUPANCY_PORT);
		cJSON_AddNumberToObject(occupancy, "intervalMinutes", 15);
		cJSON_AddItemToObject(occupancy, "selectedLabels", cJSON_CreateArray());
		cJSON_DeleteItemFromObject(transmission, "occupancy");
		cJSON_AddItemToObject(transmission, "occupancy", occupancy);
	}
	cJSON* occupancy = Ensure_Object(transmission, "occupancy");
	Replace_Number(occupancy, "port", OCCUPANCY_PORT);
	if (!cJSON_IsArray(cJSON_GetObjectItem(occupancy, "selectedLabels"))) {
		cJSON_DeleteItemFromObject(occupancy, "selectedLabels");
		cJSON_AddItemToObject(occupancy, "selectedLabels", cJSON_CreateArray());
	}
	cJSON* detection = Ensure_Object(g_settings, "detection");
	cJSON* capture_mode = cJSON_GetObjectItem(detection, "captureMode");
	if (!cJSON_IsString(capture_mode) ||
		(strcmp(capture_mode->valuestring, "balanced") != 0 &&
		 strcmp(capture_mode->valuestring, "crop") != 0 &&
		 strcmp(capture_mode->valuestring, "letterbox") != 0))
		Replace_String(detection, "captureMode", "balanced");
	if (!cJSON_IsNumber(cJSON_GetObjectItem(detection, "objectness"))) cJSON_AddNumberToObject(detection, "objectness", 0.25);
	if (!cJSON_IsNumber(cJSON_GetObjectItem(detection, "confidence"))) cJSON_AddNumberToObject(detection, "confidence", 0.30);
	if (!cJSON_IsNumber(cJSON_GetObjectItem(detection, "nms"))) cJSON_AddNumberToObject(detection, "nms", 0.45);
	if (!cJSON_IsNumber(cJSON_GetObjectItem(detection, "minimumWidth"))) cJSON_AddNumberToObject(detection, "minimumWidth", 0);
	if (!cJSON_IsNumber(cJSON_GetObjectItem(detection, "minimumHeight"))) cJSON_AddNumberToObject(detection, "minimumHeight", 0);
	if (!cJSON_IsArray(cJSON_GetObjectItem(detection, "includeArea"))) cJSON_AddItemToObject(detection, "includeArea", cJSON_CreateArray());
	if (!cJSON_IsArray(cJSON_GetObjectItem(detection, "selectedLabels"))) {
		cJSON* selected = cJSON_Duplicate(cJSON_GetObjectItem(occupancy, "selectedLabels"), 1);
		cJSON_AddItemToObject(detection, "selectedLabels", selected ? selected : cJSON_CreateArray());
	}
	cJSON_DeleteItemFromObject(detection, "excludeAreas");
	ACAP_FILE_Write("localdata/settings.json", g_settings);
}

static int Point_In_Polygon(cJSON* points, double x, double y) {
	int count = cJSON_IsArray(points) ? cJSON_GetArraySize(points) : 0;
	if (count < 3) return 0;
	int inside = 0;
	for (int index = 0, previous = count - 1; index < count; previous = index++) {
		cJSON* a = cJSON_GetArrayItem(points, index);
		cJSON* b = cJSON_GetArrayItem(points, previous);
		double ax = cJSON_GetObjectItem(a, "x") ? cJSON_GetObjectItem(a, "x")->valuedouble : 0;
		double ay = cJSON_GetObjectItem(a, "y") ? cJSON_GetObjectItem(a, "y")->valuedouble : 0;
		double bx = cJSON_GetObjectItem(b, "x") ? cJSON_GetObjectItem(b, "x")->valuedouble : 0;
		double by = cJSON_GetObjectItem(b, "y") ? cJSON_GetObjectItem(b, "y")->valuedouble : 0;
		if ((ay > y) != (by > y) && x < (bx - ax) * (y - ay) / ((by - ay) == 0 ? 1e-9 : by - ay) + ax) inside = !inside;
	}
	return inside;
}

static int Detection_Passes(cJSON* detection, cJSON* config, int width, int height) {
	cJSON* selected_labels = cJSON_GetObjectItem(config, "selectedLabels");
	cJSON* detection_label = cJSON_GetObjectItem(detection, "label");
	if (cJSON_IsArray(selected_labels) && cJSON_GetArraySize(selected_labels) > 0) {
		int selected = 0;
		cJSON* label = NULL;
		cJSON_ArrayForEach(label, selected_labels) {
			if (cJSON_IsString(label) && cJSON_IsString(detection_label) &&
				strcmp(label->valuestring, detection_label->valuestring) == 0) selected = 1;
		}
		if (!selected) return 0;
	}
	double x = cJSON_GetObjectItem(detection, "x")->valuedouble;
	double y = cJSON_GetObjectItem(detection, "y")->valuedouble;
	double w = cJSON_GetObjectItem(detection, "w")->valuedouble;
	double h = cJSON_GetObjectItem(detection, "h")->valuedouble;
	double center_x = (x + w / 2) * width;
	double center_y = (y + h / 2) * height;
	cJSON* minimum_width = cJSON_GetObjectItem(config, "minimumWidth");
	cJSON* minimum_height = cJSON_GetObjectItem(config, "minimumHeight");
	if (w * width < (minimum_width ? minimum_width->valuedouble : 0) ||
		h * height < (minimum_height ? minimum_height->valuedouble : 0)) return 0;
	cJSON* include = cJSON_GetObjectItem(config, "includeArea");
	if (cJSON_IsArray(include) && cJSON_GetArraySize(include) >= 3 && !Point_In_Polygon(include, center_x, center_y)) return 0;
	return 1;
}

static gboolean Process_Frame(gpointer unused) {
	(void)unused;
	VdoBuffer* frame = Video_Capture_YUV();
	if (!frame) {
		ACAP_STATUS_SetString("model", "error", "Video capture failed");
		return G_SOURCE_CONTINUE;
	}
	cJSON* detections = Model_Inference(frame);
	cJSON* filtered = cJSON_CreateArray();
	pthread_mutex_lock(&g_runtime_mutex);
	cJSON* detection_settings = g_detection_config ? cJSON_Duplicate(g_detection_config, 1) : cJSON_CreateObject();
	pthread_mutex_unlock(&g_runtime_mutex);
	const cJSON* model = Model_Get_Config();
	int width = model ? cJSON_GetObjectItem(model, "modelWidth")->valueint : 640;
	int height = model ? cJSON_GetObjectItem(model, "modelHeight")->valueint : 640;
	cJSON* detection = NULL;
	cJSON_ArrayForEach(detection, detections) {
		if (Detection_Passes(detection, detection_settings, width, height))
			cJSON_AddItemToArray(filtered, cJSON_Duplicate(detection, 1));
	}
	cJSON_Delete(detections);
	filtered = Model_Apply_NMS(filtered);
	size_t count = (size_t)cJSON_GetArraySize(filtered);
	const char** labels = count ? calloc(count, sizeof(char*)) : NULL;
	for (size_t index = 0; index < count; index++) {
		cJSON* label = cJSON_GetObjectItem(cJSON_GetArrayItem(filtered, (int)index), "label");
		labels[index] = cJSON_IsString(label) ? label->valuestring : NULL;
	}
	Occupancy_Record_Frame(labels, count);
	free(labels);
	ACAP_STATUS_SetObject("detections", "objects", filtered);
	ACAP_STATUS_SetNumber("detections", "count", (int)count);
	ACAP_STATUS_SetString("detections", "lastFrame", ACAP_DEVICE_Local_Time());
	Update_Occupancy_Status();
	cJSON_Delete(filtered);
	cJSON_Delete(detection_settings);
	return G_SOURCE_CONTINUE;
}

static int Publish_Occupancy(int force) {
	pthread_mutex_lock(&g_publish_mutex);
	int allowed = g_selection_valid && (force || g_publish_enabled);
	pthread_mutex_unlock(&g_publish_mutex);
	if (!allowed) return 0;
	OccupancyPublishSnapshot snapshot;
	if (!Occupancy_Begin_Publish(&snapshot)) return 0;
	unsigned char payload[OCCUPANCY_MAX_LABELS];
	size_t length = Occupancy_Build_Payload(&snapshot, payload, sizeof(payload));
	int success = length > 0 && B100_Send_Bytes(payload, (int)length, OCCUPANCY_PORT, 0);
	const char* error = success ? NULL : B100_Get_Last_Error();
	Occupancy_Finish_Publish(snapshot.token, success);
	Append_Publish_Log(payload, length, success, error);
	if (success) {
		pthread_mutex_lock(&g_publish_mutex);
		g_last_publish = time(NULL);
		g_next_publish = g_last_publish + g_interval_minutes * 60;
		pthread_mutex_unlock(&g_publish_mutex);
	}
	Update_Occupancy_Status();
	return success;
}

static void* Publish_Thread(void* unused) {
	(void)unused;
	while (g_running) {
		pthread_mutex_lock(&g_publish_mutex);
		int due = g_publish_enabled && g_selection_valid && time(NULL) >= g_next_publish;
		pthread_mutex_unlock(&g_publish_mutex);
		if (due) Publish_Occupancy(0);
		for (int second = 0; second < 1 && g_running; second++) sleep(1);
	}
	return NULL;
}

static void Update_Bridge_Status(B100_Status* status) {
	if (!status) return;
	ACAP_STATUS_SetBool("bridge", "connected", status->connected == B100_CONNECTED);
	ACAP_STATUS_SetString("bridge", "hardware", status->hardware);
	ACAP_STATUS_SetString("bridge", "hardwareVersion", status->hardwareVersion);
	ACAP_STATUS_SetString("bridge", "firmwareVersion", status->firmwareVersion);
	ACAP_STATUS_SetString("bridge", "powerSource", status->powerSource);
	ACAP_STATUS_SetString("bridge", "ipAddr", status->ipAddr);
	ACAP_STATUS_SetString("bridge", "devEUI", status->devEUI);
	ACAP_STATUS_SetString("bridge", "devAddr", status->devAddrStr);
	ACAP_STATUS_SetNumber("bridge", "restartCounter", status->restartCounter);
	ACAP_STATUS_SetNumber("bridge", "tempC", status->tempC);
	ACAP_STATUS_SetBool("bridge", "dhcpEnabled", status->dhcpEnabled);
	ACAP_STATUS_SetBool("bridge", "mqttEnabled", status->mqttEnabled);
	ACAP_STATUS_SetBool("bridge", "httpApiEnabled", status->httpApiEnabled);
	ACAP_STATUS_SetString("bridge", "callbackStatus", status->callbackStatus);
	ACAP_STATUS_SetNumber("bridge", "tUnix", status->tUnix);
	ACAP_STATUS_SetNumber("bridge", "tamper", status->tamper);
	ACAP_STATUS_SetBool("bridge", "callbacksActive", g_callbacks_configured);
	ACAP_STATUS_SetBool("lorawan", "connected", status->connected == B100_CONNECTED);
	ACAP_STATUS_SetBool("lorawan", "joined", status->joined);
	if (status->hasStatusCode) ACAP_STATUS_SetNumber("lorawan", "statusCode", status->statusCode);
	ACAP_STATUS_SetString("lorawan", "statusText", status->statusText);
	if (status->hasFcntUp) ACAP_STATUS_SetNumber("lorawan", "fcntUp", status->fcntUp);
	if (status->hasFcntDown) ACAP_STATUS_SetNumber("lorawan", "fcntDown", status->fcntDown);
	if (status->hasDataRate) ACAP_STATUS_SetNumber("lorawan", "dataRate", status->dataRate);
	if (status->hasMaxPayload) ACAP_STATUS_SetNumber("lorawan", "maxPayload", status->maxPayload);
	if (status->hasRssi) ACAP_STATUS_SetNumber("lorawan", "rssi", status->rssi);
	if (status->hasSnr) ACAP_STATUS_SetNumber("lorawan", "snr", status->snr);
	if (status->hasNextUploadMs) ACAP_STATUS_SetNumber("lorawan", "nextUploadMs", status->nextUploadMs);
	if (status->hasMargin) ACAP_STATUS_SetNumber("lorawan", "margin", status->margin);
	if (status->hasGwCount) ACAP_STATUS_SetNumber("lorawan", "gwCount", status->gwCount);
}

static int Configure_Callbacks(void) {
	char status_uri[64], receive_uri[64], gps_uri[64];
	char callback_ip[sizeof(g_callback_ip)], callback_user[sizeof(g_callback_user)];
	char callback_password[sizeof(g_callback_password)];
	int callback_port;
	pthread_mutex_lock(&g_runtime_mutex);
	snprintf(callback_ip, sizeof(callback_ip), "%s", g_callback_ip);
	snprintf(callback_user, sizeof(callback_user), "%s", g_callback_user);
	snprintf(callback_password, sizeof(callback_password), "%s", g_callback_password);
	callback_port = g_callback_port;
	pthread_mutex_unlock(&g_runtime_mutex);
	const char* address = callback_ip[0] ? callback_ip : ACAP_DEVICE_Prop("IPv4");
	snprintf(status_uri, sizeof(status_uri), "/local/%s/%s", APP_PACKAGE, CALLBACK_STATUS);
	snprintf(receive_uri, sizeof(receive_uri), "/local/%s/%s", APP_PACKAGE, CALLBACK_RECEIVE);
	snprintf(gps_uri, sizeof(gps_uri), "/local/%s/%s", APP_PACKAGE, CALLBACK_GPS);
	int configured = B100_Configure_Callbacks(address, callback_port, status_uri, receive_uri,
		callback_user, callback_password);
	pthread_mutex_lock(&g_runtime_mutex);
	g_callbacks_configured = configured;
	pthread_mutex_unlock(&g_runtime_mutex);
	if (g_callbacks_configured) B100_Configure_GPS_Callback(gps_uri, 60);
	ACAP_STATUS_SetBool("bridge", "callbacksActive", g_callbacks_configured);
	return g_callbacks_configured;
}

static void B100_Status_Handler(B100_Status* status) {
	Update_Bridge_Status(status);
}

static void B100_GPS_Handler(B100_GPS* gps) {
	if (!gps) return;
	ACAP_STATUS_SetNumber("gps", "gps_status", gps->gps_status);
	ACAP_STATUS_SetString("gps", "ns", gps->ns);
	ACAP_STATUS_SetNumber("gps", "lat", gps->lat);
	ACAP_STATUS_SetString("gps", "ew", gps->ew);
	ACAP_STATUS_SetNumber("gps", "lon", gps->lon);
	ACAP_STATUS_SetNumber("gps", "alt", gps->alt);
	ACAP_STATUS_SetNumber("gps", "nosv", gps->nosv);
	ACAP_STATUS_SetNumber("gps", "pdop", gps->pdop);
	ACAP_STATUS_SetNumber("gps", "hdop", gps->hdop);
	ACAP_STATUS_SetNumber("gps", "vdop", gps->vdop);
	ACAP_STATUS_SetString("gps", "utc", gps->utc);
	ACAP_STATUS_SetString("gps", "date", gps->date);
	ACAP_STATUS_SetNumber("gps", "sog", gps->sog);
	ACAP_STATUS_SetNumber("gps", "cog", gps->cog);
}

static int Downlink_Bytes(const B100_Downlink* downlink, unsigned char* bytes, size_t capacity) {
	if (strcmp(downlink->payload_type, "HEX") != 0) {
		size_t length = (size_t)downlink->length;
		if (length > capacity) return 0;
		memcpy(bytes, downlink->payload, length);
		return (int)length;
	}
	size_t chars = strlen(downlink->payload);
	if (chars % 2 || chars / 2 > capacity) return 0;
	for (size_t index = 0; index < chars; index += 2) {
		int high = Hex_Nibble(downlink->payload[index]);
		int low = Hex_Nibble(downlink->payload[index + 1]);
		if (high < 0 || low < 0) return 0;
		bytes[index / 2] = (unsigned char)((high << 4) | low);
	}
	return (int)(chars / 2);
}

static void B100_Downlink_Handler(B100_Downlink* downlink) {
	if (!downlink) return;
	cJSON* entry = cJSON_CreateObject();
	cJSON_AddStringToObject(entry, "time", ACAP_DEVICE_Local_Time());
	cJSON_AddNumberToObject(entry, "port", downlink->port);
	cJSON_AddNumberToObject(entry, "length", downlink->length);
	cJSON_AddStringToObject(entry, "payload", downlink->payload);
	cJSON_AddStringToObject(entry, "payload_type", downlink->payload_type);
	cJSON_AddNumberToObject(entry, "rssi", downlink->rssi);
	cJSON_AddNumberToObject(entry, "snr", downlink->snr);
	cJSON_AddNumberToObject(entry, "fcntDown", downlink->fcntDown);
	pthread_mutex_lock(&g_log_mutex);
	if (!g_downlink_log) g_downlink_log = cJSON_CreateArray();
	cJSON_AddItemToArray(g_downlink_log, entry);
	while (cJSON_GetArraySize(g_downlink_log) > LOG_LIMIT) cJSON_DeleteItemFromArray(g_downlink_log, 0);
	ACAP_STATUS_SetObject("lorawan", "downlinks", g_downlink_log);
	pthread_mutex_unlock(&g_log_mutex);
	unsigned char bytes[256];
	int length = Downlink_Bytes(downlink, bytes, sizeof(bytes));
	if (length < 1) return;
	if (downlink->port == 100) {
		if (bytes[0] == 1) B100_Restart();
		else if (bytes[0] == 2) B100_Join_Auto();
		else if (bytes[0] == 3) Publish_Occupancy(1);
	} else if (downlink->port == 110 && length >= 2) {
		if (bytes[0] == 1) {
			int interval = Clamp(bytes[1], 1, 60);
			pthread_mutex_lock(&g_publish_mutex);
			g_interval_minutes = interval;
			g_next_publish = time(NULL) + interval * 60;
			pthread_mutex_unlock(&g_publish_mutex);
			cJSON* transmission = Ensure_Object(g_settings, "transmission");
			cJSON* occupancy = Ensure_Object(transmission, "occupancy");
			Replace_Number(occupancy, "intervalMinutes", interval);
			ACAP_FILE_Write("localdata/settings.json", g_settings);
			Update_Occupancy_Status();
		} else if (bytes[0] == 2 && bytes[1] <= 5) {
			cJSON* params = cJSON_CreateObject();
			cJSON_AddNumberToObject(params, "data_rate", bytes[1]);
			B100_Set_Params(params);
			cJSON_Delete(params);
		} else if (bytes[0] == 3) {
			cJSON* params = cJSON_CreateObject();
			cJSON_AddNumberToObject(params, "adr_enable", bytes[1] ? 1 : 0);
			B100_Set_Params(params);
			cJSON_Delete(params);
		}
	} else if (downlink->port == 120) {
		char information[220];
		if (bytes[0] == 1) {
			snprintf(information, sizeof(information), "%s,%s,%s,%dh,2.0.0",
				ACAP_DEVICE_Prop("model"), ACAP_DEVICE_Prop("serial"), ACAP_DEVICE_Prop("firmware"),
				(int)(ACAP_DEVICE_Uptime() / 3600));
			B100_Send(information, 121, 0);
		} else if (bytes[0] == 2) {
			B100_Status* status = B100_Get_Status();
			snprintf(information, sizeof(information), "%s/%s,%s,%s,%.0fC,R%u,%s", status->hardware,
				status->hardwareVersion, status->firmwareVersion, status->powerSource, status->tempC,
				status->restartCounter, status->devAddrStr);
			B100_Send(information, 122, 0);
		} else if (bytes[0] == 3) Update_Bridge_Status(B100_Get_Status());
	}
}

static void* Health_Thread(void* unused) {
	(void)unused;
	while (g_running) {
		B100_Fetch_Device_Info();
		B100_Status* status = B100_Get_Status();
		Update_Bridge_Status(status);
		if (status->connected == B100_CONNECTED) {
			if (!g_callbacks_configured || !status->httpApiEnabled) Configure_Callbacks();
			if (g_callbacks_configured) B100_Request_Status();
			cJSON* gps = B100_Get_GPS();
			cJSON_Delete(gps);
			if (!status->joined) {
				pthread_mutex_lock(&g_runtime_mutex);
				int auto_join = g_auto_join;
				pthread_mutex_unlock(&g_runtime_mutex);
				if (auto_join) B100_Join_Auto();
			}
		}
		pthread_mutex_lock(&g_runtime_mutex);
		int health_interval = g_health_interval;
		pthread_mutex_unlock(&g_runtime_mutex);
		for (int second = 0; second < health_interval && g_running; second++) sleep(1);
	}
	return NULL;
}

static int Require_Post(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (method && strcmp(method, "POST") == 0) return 1;
	ACAP_HTTP_Respond_Error(response, 405, "Method must be POST");
	return 0;
}

static cJSON* Request_JSON(ACAP_HTTP_Request request) {
	cJSON* body = ACAP_HTTP_Request_JSON(request, NULL);
	if (!body) {
		const char* raw = ACAP_HTTP_Get_Body(request);
		size_t length = ACAP_HTTP_Get_Body_Length(request);
		if (raw && length) body = cJSON_ParseWithLength(raw, length);
	}
	return body;
}

static void Endpoint_Join(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if (!Require_Post(response, request)) return;
	char* dr_string = ACAP_HTTP_Request_Param(request, "drJoin");
	char* adr_string = ACAP_HTTP_Request_Param(request, "adr");
	char* up_string = ACAP_HTTP_Request_Param(request, "drUp");
	int success = B100_Join(dr_string ? atoi(dr_string) : 0, adr_string ? atoi(adr_string) : 1,
		up_string ? atoi(up_string) : 4);
	free(dr_string); free(adr_string); free(up_string);
	if (success) ACAP_HTTP_Respond_Text(response, "Join request sent");
	else ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
}

static void Endpoint_Test(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if (!Require_Post(response, request)) return;
	if (B100_Test_Connection()) ACAP_HTTP_Respond_Text(response, "Connection successful");
	else ACAP_HTTP_Respond_Error(response, 503, B100_Get_Last_Error());
}

static void Endpoint_Restart(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if (!Require_Post(response, request)) return;
	if (B100_Restart()) ACAP_HTTP_Respond_Text(response, "Restart initiated");
	else ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
}

static void Endpoint_Send(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if (!Require_Post(response, request)) return;
	char* payload = ACAP_HTTP_Request_Param(request, "payload");
	char* port_text = ACAP_HTTP_Request_Param(request, "port");
	char* confirmed_text = ACAP_HTTP_Request_Param(request, "confirmed");
	if (!payload) ACAP_HTTP_Respond_Error(response, 400, "Missing payload");
	else if (B100_Send(payload, port_text ? atoi(port_text) : OCCUPANCY_PORT,
		confirmed_text ? atoi(confirmed_text) : 0)) ACAP_HTTP_Respond_Text(response, "Message sent");
	else ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	free(payload); free(port_text); free(confirmed_text);
}

static void Endpoint_Publish(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if (!Require_Post(response, request)) return;
	if (Publish_Occupancy(1)) ACAP_HTTP_Respond_Text(response, "Occupancy publish accepted");
	else ACAP_HTTP_Respond_Error(response, 500, "Publish failed or selected labels are invalid");
}

static void Endpoint_Counters(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	(void)request;
	OccupancyStatus status;
	Occupancy_Get_Status(&status);
	cJSON* root = cJSON_CreateObject();
	cJSON_AddBoolToObject(root, "selectionValid", g_selection_valid);
	cJSON_AddNumberToObject(root, "port", OCCUPANCY_PORT);
	cJSON_AddNumberToObject(root, "nextPublish", (double)g_next_publish);
	cJSON* labels = cJSON_AddArrayToObject(root, "labels");
	for (size_t index = 0; index < status.label_count; index++) {
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "byte", (int)index);
		cJSON_AddStringToObject(item, "label", status.labels[index]);
		cJSON_AddNumberToObject(item, "current", status.current[index]);
		cJSON_AddNumberToObject(item, "maximum", status.maxima[index]);
		cJSON_AddNumberToObject(item, "lastPublished", status.last_published[index]);
		cJSON_AddItemToArray(labels, item);
	}
	ACAP_HTTP_Respond_JSON(response, root);
	cJSON_Delete(root);
}

static void Endpoint_Model(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (!method || strcmp(method, "GET") != 0) {
		ACAP_HTTP_Respond_Error(response, 405, "Model is fixed at build time");
		return;
	}
	const cJSON* config = Model_Get_Config();
	cJSON* payload = config ? cJSON_Duplicate(config, 1) : cJSON_CreateObject();
	ACAP_HTTP_Respond_JSON(response, payload);
	cJSON_Delete(payload);
}

static void Endpoint_B100_Callback(ACAP_HTTP_Response response, ACAP_HTTP_Request request, int kind) {
	if (!Require_Post(response, request)) return;
	cJSON* body = Request_JSON(request);
	ACAP_HTTP_Respond_Text(response, "OK");
	if (!body) return;
	if (kind == 0) B100_Process_Status_Callback(body);
	else if (kind == 1) B100_Process_Receive_Callback(body);
	else B100_Process_GPS_Callback(body);
	cJSON_Delete(body);
}

static void Endpoint_Status_Callback(ACAP_HTTP_Response response, ACAP_HTTP_Request request) { Endpoint_B100_Callback(response, request, 0); }
static void Endpoint_Receive_Callback(ACAP_HTTP_Response response, ACAP_HTTP_Request request) { Endpoint_B100_Callback(response, request, 1); }
static void Endpoint_GPS_Callback(ACAP_HTTP_Response response, ACAP_HTTP_Request request) { Endpoint_B100_Callback(response, request, 2); }

static void Endpoint_B100_Info(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	(void)request;
	cJSON* info = B100_Get_Info();
	if (!info) { ACAP_HTTP_Respond_Error(response, 503, "B100 not reachable"); return; }
	cJSON* params = B100_Get_Params(NULL);
	if (params) {
		const char* names[] = {"callback_addr", "callback_port", "callback_digest_user",
			"callback_status_uri", "callback_receive_uri"};
		const char* aliases[] = {"callback_configured_addr", "callback_configured_port", "callback_digest_user",
			"callback_status_uri", "callback_receive_uri"};
		for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
			cJSON* value = cJSON_GetObjectItem(params, names[index]);
			if (cJSON_IsString(value)) cJSON_AddStringToObject(info, aliases[index], value->valuestring);
			else if (cJSON_IsNumber(value)) cJSON_AddNumberToObject(info, aliases[index], value->valuedouble);
		}
		cJSON_Delete(params);
	}
	cJSON_AddBoolToObject(info, "callbacks_active", g_callbacks_configured);
	ACAP_HTTP_Respond_JSON(response, info);
	cJSON_Delete(info);
}

static void Endpoint_B100_Params(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	const char* method = ACAP_HTTP_Get_Method(request);
	if (method && strcmp(method, "GET") == 0) {
		cJSON* params = B100_Get_Params(NULL);
		if (params) { ACAP_HTTP_Respond_JSON(response, params); cJSON_Delete(params); }
		else ACAP_HTTP_Respond_Error(response, 503, B100_Get_Last_Error());
	} else if (method && strcmp(method, "POST") == 0) {
		cJSON* params = Request_JSON(request);
		if (params && B100_Set_Params(params)) ACAP_HTTP_Respond_Text(response, "Parameters updated");
		else ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
		cJSON_Delete(params);
	} else ACAP_HTTP_Respond_Error(response, 405, "Method must be GET or POST");
}

static void Endpoint_Request_Status(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if (!Require_Post(response, request)) return;
	if (!Configure_Callbacks()) ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	else if (B100_Request_Status()) ACAP_HTTP_Respond_Text(response, "Callbacks configured and status requested");
	else ACAP_HTTP_Respond_Text(response, "Callbacks configured; status request was not accepted");
}

static void Endpoint_Linkcheck(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	if (!Require_Post(response, request)) return;
	if (!Configure_Callbacks()) ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
	else if (B100_Link_Check()) ACAP_HTTP_Respond_Text(response, "Linkcheck request sent");
	else ACAP_HTTP_Respond_Error(response, 500, B100_Get_Last_Error());
}

static void Endpoint_GPS(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	(void)request;
	cJSON* gps = B100_Get_GPS();
	if (gps) { ACAP_HTTP_Respond_JSON(response, gps); cJSON_Delete(gps); }
	else ACAP_HTTP_Respond_Error(response, 503, "GPS unavailable");
}

static void Endpoint_Translator(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	OTA_Translator_Data(response, request);
}

static gboolean Signal_Handler(gpointer unused) {
	(void)unused;
	if (g_main_loop) g_main_loop_quit(g_main_loop);
	return G_SOURCE_REMOVE;
}

static void Register_Endpoints(void) {
	ACAP_HTTP_Node("join", Endpoint_Join);
	ACAP_HTTP_Node("test", Endpoint_Test);
	ACAP_HTTP_Node("restart", Endpoint_Restart);
	ACAP_HTTP_Node("send", Endpoint_Send);
	ACAP_HTTP_Node("publish", Endpoint_Publish);
	ACAP_HTTP_Node("counters", Endpoint_Counters);
	ACAP_HTTP_Node("model", Endpoint_Model);
	ACAP_HTTP_Node("translator", Endpoint_Translator);
	ACAP_HTTP_Node("ota_encoder", OTA_Translator_Encoder);
	ACAP_HTTP_Node("ota_decoder", OTA_Translator_Decoder);
	ACAP_HTTP_Node(CALLBACK_STATUS, Endpoint_Status_Callback);
	ACAP_HTTP_Node(CALLBACK_RECEIVE, Endpoint_Receive_Callback);
	ACAP_HTTP_Node(CALLBACK_GPS, Endpoint_GPS_Callback);
	ACAP_HTTP_Node("b100_info", Endpoint_B100_Info);
	ACAP_HTTP_Node("b100_params", Endpoint_B100_Params);
	ACAP_HTTP_Node("b100_request_status", Endpoint_Request_Status);
	ACAP_HTTP_Node("linkcheck", Endpoint_Linkcheck);
	ACAP_HTTP_Node("gps", Endpoint_GPS);
}

int main(void) {
	setbuf(stdout, NULL);
	openlog(APP_PACKAGE, LOG_PID | LOG_CONS, LOG_USER);
	g_settings = ACAP_Init(APP_PACKAGE, Settings_Updated);
	if (!g_settings) return 1;
	Load_Bridge_Settings(cJSON_GetObjectItem(g_settings, "b100"));
	cJSON* polling = cJSON_GetObjectItem(g_settings, "polling");
	if (polling) Settings_Updated("polling", polling);
	Migrate_Settings();
	cJSON* model = Model_Setup(cJSON_GetObjectItem(g_settings, "detection"));
	Configure_Occupancy(cJSON_GetObjectItem(g_settings, "transmission"), 1);
	Settings_Updated("detection", cJSON_GetObjectItem(g_settings, "detection"));
	if (model) {
		unsigned width = (unsigned)cJSON_GetObjectItem(model, "videoWidth")->valueint;
		unsigned height = (unsigned)cJSON_GetObjectItem(model, "videoHeight")->valueint;
		const char* image_fit = cJSON_GetObjectItem(model, "imageFit")->valuestring;
		if (Video_Start_YUV(width, height, image_fit)) g_idle_add(Process_Frame, NULL);
		else ACAP_STATUS_SetString("model", "error", "Unable to start VDO stream");
	}
	char bridge_ip[sizeof(g_b100_ip)], api_user[sizeof(g_api_user)], api_password[sizeof(g_api_password)];
	int bridge_port;
	pthread_mutex_lock(&g_runtime_mutex);
	snprintf(bridge_ip, sizeof(bridge_ip), "%s", g_b100_ip);
	snprintf(api_user, sizeof(api_user), "%s", g_api_user);
	snprintf(api_password, sizeof(api_password), "%s", g_api_password);
	bridge_port = g_b100_port;
	pthread_mutex_unlock(&g_runtime_mutex);
	B100_Init(bridge_ip, bridge_port, 30);
	B100_Set_API_Credentials(api_user, api_password);
	B100_Set_Downlink_Callback(B100_Downlink_Handler);
	B100_Set_Status_Callback(B100_Status_Handler);
	B100_Set_GPS_Callback(B100_GPS_Handler);
	Register_Endpoints();
	if (B100_Test_Connection()) {
		Configure_Callbacks();
		if (g_callbacks_configured) B100_Request_Status();
	}
	pthread_create(&g_publish_thread, NULL, Publish_Thread, NULL);
	pthread_create(&g_health_thread, NULL, Health_Thread, NULL);
	ACAP_STATUS_SetString("app", "status", "Running");
	g_main_loop = g_main_loop_new(NULL, FALSE);
	GSource* signal_source = g_unix_signal_source_new(SIGTERM);
	g_source_set_callback(signal_source, Signal_Handler, NULL, NULL);
	g_source_attach(signal_source, NULL);
	g_main_loop_run(g_main_loop);
	g_running = 0;
	pthread_join(g_publish_thread, NULL);
	pthread_join(g_health_thread, NULL);
	ACAP_Cleanup();
	Video_Stop_YUV();
	Model_Cleanup();
	pthread_mutex_lock(&g_runtime_mutex);
	cJSON_Delete(g_detection_config);
	g_detection_config = NULL;
	pthread_mutex_unlock(&g_runtime_mutex);
	B100_Cleanup();
	closelog();
	return 0;
}