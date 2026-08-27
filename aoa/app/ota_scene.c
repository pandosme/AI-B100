#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ACAP.h"
#include "counter.h"
#include "occupancy.h"
#include "presence.h"
#include "ota_scene.h"

#define OTA_PORT_COUNTING 131
#define OTA_PORT_OCCUPANCY 132
#define OTA_PORT_PRESENCE 133
#define OTA_SCENE_MAX_COUNT 10
#define OTA_SCENE_MAX_POINTS 10
#define OTA_SCENE_PENDING_TIMEOUT 120
#define OTA_SCENE_CONFIG_VERSION_Q15 1
#define OTA_SCENE_CONFIG_VERSION_INTEGER 2
#define PRESENCE_SCHEDULE_DEFAULT_START_MINUTES (18 * 60)
#define PRESENCE_SCHEDULE_DEFAULT_END_MINUTES (6 * 60)

typedef enum {
	SCENE_DOMAIN_COUNTING,
	SCENE_DOMAIN_OCCUPANCY,
	SCENE_DOMAIN_PRESENCE
} SceneDomain;

typedef struct {
	cJSON* json;
	int id;
	const char* name;
	uint16_t fingerprint;
	int point_count;
} SceneEntry;

typedef struct {
	SceneEntry entries[OTA_SCENE_MAX_COUNT];
	int count;
	uint16_t fingerprint;
} SceneMap;

typedef struct {
	int active;
	time_t updated_at;
	uint8_t transaction_id;
	uint8_t config_version;
	uint8_t scene_index;
	uint16_t scene_id;
	uint16_t scene_fingerprint;
	uint16_t map_fingerprint;
	uint8_t page_count;
	uint8_t total_points;
	uint16_t received_pages;
	uint8_t fields[9];
	int16_t points[OTA_SCENE_MAX_POINTS][2];
} PendingSceneUpdate;

static PendingSceneUpdate g_pending[3];
static pthread_mutex_t g_scene_ota_mutex = PTHREAD_MUTEX_INITIALIZER;

static cJSON* Settings_Scene(cJSON* settings, SceneDomain domain, const char* name, int create);

static uint16_t Read_U16(const uint8_t* data) {
	return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static void Write_U16(uint8_t* data, uint16_t value) {
	data[0] = (uint8_t)(value & 0xFF);
	data[1] = (uint8_t)(value >> 8);
}

static uint16_t CRC16_Update(uint16_t crc, uint8_t value) {
	crc ^= (uint16_t)value << 8;
	for (int bit = 0; bit < 8; bit++)
		crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	return crc;
}

static uint16_t CRC16_String(uint16_t crc, const char* value) {
	for (const unsigned char* cursor = (const unsigned char*)(value ? value : ""); *cursor; cursor++)
		crc = CRC16_Update(crc, *cursor);
	return CRC16_Update(crc, 0);
}

static uint16_t Scene_Fingerprint(int id, const char* type, const char* name) {
	uint16_t crc = 0xFFFF;
	crc = CRC16_Update(crc, (uint8_t)(id & 0xFF));
	crc = CRC16_Update(crc, (uint8_t)((unsigned int)id >> 8));
	crc = CRC16_String(crc, type);
	return CRC16_String(crc, name);
}

static int Scene_Entry_Compare(const void* left, const void* right) {
	const SceneEntry* a = left;
	const SceneEntry* b = right;
	return (a->id > b->id) - (a->id < b->id);
}

static int Is_Counting_Type(const char* type) {
	return type && (strcmp(type, "crosslinecounting") == 0 || strcmp(type, "CrosslineCounting") == 0);
}

static int Is_Occupancy_Type(const char* type) {
	return type && (strcmp(type, "occupancyInArea") == 0 || strcmp(type, "OccupancyInArea") == 0);
}

static cJSON* Ensure_Object_Item(cJSON* parent, const char* key) {
	cJSON* item = parent ? cJSON_GetObjectItem(parent, key) : NULL;
	if (item && cJSON_IsObject(item)) return item;
	cJSON* replacement = cJSON_CreateObject();
	if (!parent || !replacement) return NULL;
	if (item) cJSON_ReplaceItemInObject(parent, key, replacement);
	else cJSON_AddItemToObject(parent, key, replacement);
	return replacement;
}

static int Is_Presence_Owned(cJSON* settings, const char* name) {
	cJSON* transmission = settings ? cJSON_GetObjectItem(settings, "transmission") : NULL;
	cJSON* presence = transmission ? cJSON_GetObjectItem(transmission, "presence") : NULL;
	cJSON* scenarios = presence ? cJSON_GetObjectItem(presence, "scenarios") : NULL;
	return scenarios && cJSON_IsObject(scenarios) && cJSON_GetObjectItem(scenarios, name) != NULL;
}

static cJSON* Scene_Trigger(cJSON* scene, SceneDomain domain) {
	cJSON* trigger = NULL;
	cJSON_ArrayForEach(trigger, cJSON_GetObjectItem(scene, "triggers")) {
		cJSON* vertices = cJSON_GetObjectItem(trigger, "vertices");
		if (!vertices || !cJSON_IsArray(vertices)) continue;
		if (domain == SCENE_DOMAIN_COUNTING) return trigger;
		cJSON* type = cJSON_GetObjectItem(trigger, "type");
		if (type && cJSON_IsString(type) && strcmp(type->valuestring, "includeArea") == 0) return trigger;
	}
	return NULL;
}

static int Scene_Point_Count(cJSON* scene, SceneDomain domain) {
	cJSON* trigger = Scene_Trigger(scene, domain);
	cJSON* vertices = trigger ? cJSON_GetObjectItem(trigger, "vertices") : NULL;
	return vertices && cJSON_IsArray(vertices) ? cJSON_GetArraySize(vertices) : 0;
}

static int Build_Scene_Map(cJSON* data, cJSON* settings, SceneDomain domain, SceneMap* map) {
	if (!data || !map) return 0;
	memset(map, 0, sizeof(*map));
	cJSON* scene = NULL;
	cJSON_ArrayForEach(scene, cJSON_GetObjectItem(data, "scenarios")) {
		cJSON* id = cJSON_GetObjectItem(scene, "id");
		cJSON* name = cJSON_GetObjectItem(scene, "name");
		cJSON* type = cJSON_GetObjectItem(scene, "type");
		if (!id || !cJSON_IsNumber(id) || !name || !cJSON_IsString(name) ||
		    !type || !cJSON_IsString(type)) continue;
		int include = domain == SCENE_DOMAIN_COUNTING ? Is_Counting_Type(type->valuestring) : Is_Occupancy_Type(type->valuestring);
		if (domain == SCENE_DOMAIN_OCCUPANCY) include = include && !Is_Presence_Owned(settings, name->valuestring);
		if (domain == SCENE_DOMAIN_PRESENCE) include = include && Is_Presence_Owned(settings, name->valuestring);
		if (!include || map->count >= OTA_SCENE_MAX_COUNT) continue;
		SceneEntry* entry = &map->entries[map->count++];
		entry->json = scene;
		entry->id = id->valueint;
		entry->name = name->valuestring;
		entry->fingerprint = Scene_Fingerprint(entry->id, type->valuestring, entry->name);
		entry->point_count = Scene_Point_Count(scene, domain);
	}
	qsort(map->entries, (size_t)map->count, sizeof(map->entries[0]), Scene_Entry_Compare);
	uint16_t crc = 0xFFFF;
	for (int index = 0; index < map->count; index++) {
		crc = CRC16_Update(crc, (uint8_t)(index + 1));
		crc = CRC16_Update(crc, (uint8_t)(map->entries[index].id & 0xFF));
		crc = CRC16_Update(crc, (uint8_t)((unsigned int)map->entries[index].id >> 8));
		crc = CRC16_Update(crc, (uint8_t)(map->entries[index].fingerprint & 0xFF));
		crc = CRC16_Update(crc, (uint8_t)(map->entries[index].fingerprint >> 8));
	}
	map->fingerprint = crc;
	return 1;
}

static cJSON* Fetch_AOA_Configuration(cJSON** data) {
	const char* request = "{\"apiVersion\":\"1.6\",\"context\":\"aib100-ota\",\"method\":\"getConfiguration\"}";
	char* text = ACAP_VAPIX_Post("/local/objectanalytics/control.cgi", request);
	cJSON* root = text ? cJSON_Parse(text) : NULL;
	free(text);
	if (!root || cJSON_GetObjectItem(root, "error")) {
		cJSON_Delete(root);
		return NULL;
	}
	*data = cJSON_GetObjectItem(root, "data");
	if (!*data || !cJSON_IsObject(*data)) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

static SceneDomain Port_Domain(int port) {
	if (port == OTA_PORT_COUNTING) return SCENE_DOMAIN_COUNTING;
	if (port == OTA_PORT_OCCUPANCY) return SCENE_DOMAIN_OCCUPANCY;
	return SCENE_DOMAIN_PRESENCE;
}

static int Static_Field_Length(SceneDomain domain) {
	return domain == SCENE_DOMAIN_PRESENCE ? 9 : 3;
}

static int Legacy_Static_Field_Length(SceneDomain domain) {
	return domain == SCENE_DOMAIN_PRESENCE ? 4 : Static_Field_Length(domain);
}

static int Coordinate_Bytes(uint8_t config_version) {
	return config_version == OTA_SCENE_CONFIG_VERSION_INTEGER ? 3 : 4;
}

static int Points_Per_Page(SceneDomain domain, uint8_t config_version) {
	return (OTA_MAX_BODY_SIZE - 13 - Static_Field_Length(domain)) /
		Coordinate_Bytes(config_version);
}

static uint8_t Classes_To_Mask(cJSON* classes) {
	static const char* names[] = {"human", "car", "bike", "bus", "truck", "other"};
	uint8_t mask = 0;
	for (int bit = 0; bit < 6; bit++) {
		cJSON* item = classes ? cJSON_GetObjectItem(classes, names[bit]) : NULL;
		if (item && cJSON_IsTrue(item)) mask |= (uint8_t)(1u << bit);
	}
	return mask;
}

static int Parse_HHMM(const char* value, uint16_t* minutes_out) {
	if (!value || !minutes_out) return 0;
	if (strlen(value) != 5 || value[2] != ':') return 0;
	if (value[0] < '0' || value[0] > '2' || value[1] < '0' || value[1] > '9' ||
		value[3] < '0' || value[3] > '5' || value[4] < '0' || value[4] > '9') return 0;
	int hours = (value[0] - '0') * 10 + (value[1] - '0');
	int minutes = (value[3] - '0') * 10 + (value[4] - '0');
	if (hours > 23) return 0;
	*minutes_out = (uint16_t)(hours * 60 + minutes);
	return 1;
}

static void Ensure_Presence_Schedule_Defaults(uint8_t* fields) {
	if (!fields) return;
	fields[4] = 0;
	Write_U16(fields + 5, PRESENCE_SCHEDULE_DEFAULT_START_MINUTES);
	Write_U16(fields + 7, PRESENCE_SCHEDULE_DEFAULT_END_MINUTES);
}

static void Set_Object_Bool(cJSON* object, const char* key, int enabled) {
	if (!object || !key) return;
	cJSON* item = cJSON_CreateBool(enabled ? 1 : 0);
	if (!item) return;
	if (cJSON_GetObjectItem(object, key)) cJSON_ReplaceItemInObject(object, key, item);
	else cJSON_AddItemToObject(object, key, item);
}

static void Set_Object_String(cJSON* object, const char* key, const char* value) {
	if (!object || !key) return;
	cJSON* item = cJSON_CreateString(value ? value : "");
	if (!item) return;
	if (cJSON_GetObjectItem(object, key)) cJSON_ReplaceItemInObject(object, key, item);
	else cJSON_AddItemToObject(object, key, item);
}

static void Update_Presence_Schedule_Settings(cJSON* settings, const char* scene_name,
	const uint8_t* fields) {
	if (!settings || !scene_name || !fields) return;
	cJSON* scene = Settings_Scene(settings, SCENE_DOMAIN_PRESENCE, scene_name, 1);
	if (!scene || !cJSON_IsObject(scene)) return;
	cJSON* schedule = Ensure_Object_Item(scene, "schedule");
	if (!schedule || !cJSON_IsObject(schedule)) return;
	Set_Object_Bool(schedule, "enabled", fields[4] != 0);
	char start[6] = "18:00";
	char end[6] = "06:00";
	uint16_t start_minutes = Read_U16(fields + 5);
	uint16_t end_minutes = Read_U16(fields + 7);
	if (start_minutes > 1439) start_minutes = PRESENCE_SCHEDULE_DEFAULT_START_MINUTES;
	if (end_minutes > 1439) end_minutes = PRESENCE_SCHEDULE_DEFAULT_END_MINUTES;
	snprintf(start, sizeof(start), "%02u:%02u", start_minutes / 60, start_minutes % 60);
	snprintf(end, sizeof(end), "%02u:%02u", end_minutes / 60, end_minutes % 60);
	Set_Object_String(schedule, "start", start);
	Set_Object_String(schedule, "end", end);
}

static cJSON* Settings_Scene(cJSON* settings, SceneDomain domain, const char* name, int create) {
	cJSON* transmission = create ? Ensure_Object_Item(settings, "transmission") : cJSON_GetObjectItem(settings, "transmission");
	const char* group_name = domain == SCENE_DOMAIN_COUNTING ? "counting" : domain == SCENE_DOMAIN_OCCUPANCY ? "occupancy" : "presence";
	cJSON* group = create ? Ensure_Object_Item(transmission, group_name) : cJSON_GetObjectItem(transmission, group_name);
	cJSON* scenarios = create ? Ensure_Object_Item(group, "scenarios") : cJSON_GetObjectItem(group, "scenarios");
	if (!scenarios) return NULL;
	cJSON* scene = cJSON_GetObjectItem(scenarios, name);
	if (!scene && create) {
		scene = cJSON_CreateObject();
		cJSON_AddItemToObject(scenarios, name, scene);
	}
	return scene;
}

static uint8_t Publish_Class_Mask(cJSON* settings, SceneDomain domain, const char* name) {
	cJSON* transmission = cJSON_GetObjectItem(settings, "transmission");
	const char* group_name = domain == SCENE_DOMAIN_COUNTING ? "counting" : "occupancy";
	cJSON* group = transmission ? cJSON_GetObjectItem(transmission, group_name) : NULL;
	cJSON* scene = Settings_Scene(settings, domain, name, 0);
	cJSON* classes = scene ? cJSON_GetObjectItem(scene, "classes") : NULL;
	if (!classes) classes = group ? cJSON_GetObjectItem(group, "classes") : NULL;
	return Classes_To_Mask(classes);
}

static uint8_t AOA_Class_Mask(cJSON* scene) {
	uint8_t mask = 0;
	cJSON* classification = NULL;
	cJSON_ArrayForEach(classification, cJSON_GetObjectItem(scene, "objectClassifications")) {
		cJSON* type = cJSON_GetObjectItem(classification, "type");
		if (!type || !cJSON_IsString(type)) continue;
		if (strcmp(type->valuestring, "human") == 0) mask |= 0x01;
		if (strcmp(type->valuestring, "vehicle") != 0) continue;
		cJSON* subtypes = cJSON_GetObjectItem(classification, "subTypes");
		if (!subtypes || cJSON_GetArraySize(subtypes) == 0) {
			mask |= 0x3E;
			continue;
		}
		cJSON* subtype = NULL;
		cJSON_ArrayForEach(subtype, subtypes) {
			cJSON* subtype_name = cJSON_GetObjectItem(subtype, "type");
			if (!subtype_name || !cJSON_IsString(subtype_name)) continue;
			if (strcmp(subtype_name->valuestring, "car") == 0) mask |= 0x02;
			else if (strcmp(subtype_name->valuestring, "motorcycle/bicycle") == 0) mask |= 0x04;
			else if (strcmp(subtype_name->valuestring, "bus") == 0) mask |= 0x08;
			else if (strcmp(subtype_name->valuestring, "truck") == 0) mask |= 0x10;
			else if (strcmp(subtype_name->valuestring, "unknown") == 0) mask |= 0x20;
		}
	}
	return mask;
}

static void Read_Static_Fields(SceneDomain domain, SceneEntry* entry, cJSON* settings, uint8_t* fields) {
	memset(fields, 0, (size_t)Static_Field_Length(domain));
	if (domain == SCENE_DOMAIN_COUNTING) {
		cJSON* trigger = Scene_Trigger(entry->json, domain);
		cJSON* direction = trigger ? cJSON_GetObjectItem(trigger, "countingDirection") : NULL;
		fields[0] = direction && cJSON_IsString(direction) && strcmp(direction->valuestring, "rightToLeft") == 0;
		fields[1] = Publish_Class_Mask(settings, domain, entry->name);
		return;
	}
	if (domain == SCENE_DOMAIN_OCCUPANCY) {
		fields[0] = Publish_Class_Mask(settings, domain, entry->name);
		cJSON* transmission = cJSON_GetObjectItem(settings, "transmission");
		cJSON* occupancy = transmission ? cJSON_GetObjectItem(transmission, "occupancy") : NULL;
		cJSON* scene = Settings_Scene(settings, domain, entry->name, 0);
		cJSON* value = scene ? cJSON_GetObjectItem(scene, "value") : NULL;
		if (!value) value = occupancy ? cJSON_GetObjectItem(occupancy, "value") : NULL;
		if (value && cJSON_IsString(value) && strcmp(value->valuestring, "min") == 0) fields[1] = 1;
		else if (value && cJSON_IsString(value) && strcmp(value->valuestring, "average") == 0) fields[1] = 2;
		return;
	}
	fields[0] = AOA_Class_Mask(entry->json);
	cJSON* threshold = cJSON_GetObjectItem(entry->json, "thresholdConfiguration");
	cJSON* thresholds = threshold ? cJSON_GetObjectItem(threshold, "thresholds") : NULL;
	cJSON* first = thresholds ? cJSON_GetArrayItem(thresholds, 0) : NULL;
	cJSON* level = first ? cJSON_GetObjectItem(first, "level") : NULL;
	cJSON* delay = threshold ? cJSON_GetObjectItem(threshold, "triggerDelay") : NULL;
	fields[1] = (uint8_t)((level && cJSON_IsNumber(level) ? level->valueint : 0) + 1);
	Write_U16(fields + 2, (uint16_t)(delay && cJSON_IsNumber(delay) ? delay->valueint : 0));
	Ensure_Presence_Schedule_Defaults(fields);
	cJSON* scenario = Settings_Scene(settings, SCENE_DOMAIN_PRESENCE, entry->name, 0);
	cJSON* schedule = scenario ? cJSON_GetObjectItem(scenario, "schedule") : NULL;
	if (!schedule || !cJSON_IsObject(schedule)) return;
	cJSON* enabled = cJSON_GetObjectItem(schedule, "enabled");
	if (enabled) fields[4] = cJSON_IsTrue(enabled) ? 1 : 0;
	uint16_t minutes;
	cJSON* start = cJSON_GetObjectItem(schedule, "start");
	if (start && cJSON_IsString(start) && Parse_HHMM(start->valuestring, &minutes))
		Write_U16(fields + 5, minutes);
	cJSON* end = cJSON_GetObjectItem(schedule, "end");
	if (end && cJSON_IsString(end) && Parse_HHMM(end->valuestring, &minutes))
		Write_U16(fields + 7, minutes);
}

static int16_t Encode_Q15(double value) {
	if (value < -1.0) value = -1.0;
	if (value > 1.0) value = 1.0;
	return (int16_t)lround(value * 32767.0);
}

static uint16_t Encode_Integer_Coordinate(double value) {
	if (value < -1.0) value = -1.0;
	if (value > 1.0) value = 1.0;
	return (uint16_t)lround((value + 1.0) * 500.0);
}

static int16_t Integer_Coordinate_To_Q15(uint16_t value) {
	return Encode_Q15((double)value / 500.0 - 1.0);
}

static void Write_Integer_Point(uint8_t* data, uint16_t x, uint16_t y) {
	uint32_t packed = (uint32_t)x | ((uint32_t)y << 10);
	data[0] = (uint8_t)(packed & 0xFF);
	data[1] = (uint8_t)((packed >> 8) & 0xFF);
	data[2] = (uint8_t)((packed >> 16) & 0x0F);
}

static int Read_Integer_Point(const uint8_t* data, uint16_t* x, uint16_t* y) {
	if ((data[2] & 0xF0) != 0) return 0;
	uint32_t packed = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
		((uint32_t)data[2] << 16);
	*x = (uint16_t)(packed & 0x3FF);
	*y = (uint16_t)((packed >> 10) & 0x3FF);
	return *x <= 1000 && *y <= 1000;
}

static OTA_Status Build_Config_Page(SceneDomain domain, SceneMap* map, int scene_index,
	int page_index, cJSON* settings, uint8_t* body, size_t* body_length) {
	if (scene_index < 1 || scene_index > map->count) return OTA_STATUS_UNKNOWN_SCENE;
	SceneEntry* entry = &map->entries[scene_index - 1];
	int min_points = domain == SCENE_DOMAIN_COUNTING ? 2 : 3;
	if (entry->point_count < min_points || entry->point_count > OTA_SCENE_MAX_POINTS)
		return OTA_STATUS_INVALID_RANGE;
	int points_per_page = Points_Per_Page(domain, OTA_SCENE_CONFIG_VERSION_INTEGER);
	int page_count = (entry->point_count + points_per_page - 1) / points_per_page;
	if (page_index < 0 || page_index >= page_count) return OTA_STATUS_INVALID_RANGE;
	int point_start = page_index * points_per_page;
	int point_count = entry->point_count - point_start;
	if (point_count > points_per_page) point_count = points_per_page;
	int static_length = Static_Field_Length(domain);

	body[0] = OTA_SCENE_CONFIG_VERSION_INTEGER;
	body[1] = (uint8_t)scene_index;
	Write_U16(body + 2, (uint16_t)entry->id);
	Write_U16(body + 4, entry->fingerprint);
	Write_U16(body + 6, map->fingerprint);
	body[8] = (uint8_t)page_index;
	body[9] = (uint8_t)page_count;
	body[10] = (uint8_t)point_start;
	body[11] = (uint8_t)point_count;
	body[12] = (uint8_t)entry->point_count;
	Read_Static_Fields(domain, entry, settings, body + 13);

	cJSON* vertices = cJSON_GetObjectItem(Scene_Trigger(entry->json, domain), "vertices");
	for (int index = 0; index < point_count; index++) {
		cJSON* point = cJSON_GetArrayItem(vertices, point_start + index);
		cJSON* x = point ? cJSON_GetArrayItem(point, 0) : NULL;
		cJSON* y = point ? cJSON_GetArrayItem(point, 1) : NULL;
		if (!x || !cJSON_IsNumber(x) || !y || !cJSON_IsNumber(y)) return OTA_STATUS_INVALID_VALUE;
		int offset = 13 + static_length + index * 3;
		Write_Integer_Point(body + offset, Encode_Integer_Coordinate(x->valuedouble),
			Encode_Integer_Coordinate(y->valuedouble));
	}
	*body_length = (size_t)(13 + static_length + point_count * 3);
	return OTA_STATUS_OK;
}

static cJSON* Classes_From_Mask(uint8_t mask) {
	static const char* names[] = {"human", "car", "bike", "bus", "truck", "other"};
	cJSON* classes = cJSON_CreateObject();
	for (int bit = 0; bit < 6; bit++) cJSON_AddBoolToObject(classes, names[bit], (mask & (1u << bit)) != 0);
	return classes;
}

static cJSON* AOA_Classifications_From_Mask(uint8_t mask) {
	static const char* vehicle_types[] = {"car", "motorcycle/bicycle", "bus", "truck", "unknown"};
	cJSON* classifications = cJSON_CreateArray();
	if (mask & 0x01) {
		cJSON* human = cJSON_CreateObject();
		cJSON_AddStringToObject(human, "type", "human");
		cJSON_AddItemToArray(classifications, human);
	}
	if (mask & 0x3E) {
		cJSON* vehicle = cJSON_CreateObject();
		cJSON_AddStringToObject(vehicle, "type", "vehicle");
		cJSON* subtypes = cJSON_CreateArray();
		for (int bit = 1; bit < 6; bit++) {
			if (!(mask & (1u << bit))) continue;
			cJSON* subtype = cJSON_CreateObject();
			cJSON_AddStringToObject(subtype, "type", vehicle_types[bit - 1]);
			cJSON_AddItemToArray(subtypes, subtype);
		}
		cJSON_AddItemToObject(vehicle, "subTypes", subtypes);
		cJSON_AddItemToArray(classifications, vehicle);
	}
	return classifications;
}

static int Replace_Vertices(cJSON* scene, SceneDomain domain, PendingSceneUpdate* pending) {
	cJSON* trigger = Scene_Trigger(scene, domain);
	if (!trigger) return 0;
	cJSON* vertices = cJSON_CreateArray();
	if (!vertices) return 0;
	for (int index = 0; index < pending->total_points; index++) {
		cJSON* point = cJSON_CreateArray();
		cJSON_AddItemToArray(point, cJSON_CreateNumber(pending->points[index][0] / 32767.0));
		cJSON_AddItemToArray(point, cJSON_CreateNumber(pending->points[index][1] / 32767.0));
		cJSON_AddItemToArray(vertices, point);
	}
	if (cJSON_GetObjectItem(trigger, "vertices")) cJSON_ReplaceItemInObject(trigger, "vertices", vertices);
	else cJSON_AddItemToObject(trigger, "vertices", vertices);
	return 1;
}

static int Apply_AOA_Configuration(cJSON* data) {
	cJSON* request = cJSON_CreateObject();
	cJSON_AddStringToObject(request, "apiVersion", "1.6");
	cJSON_AddStringToObject(request, "context", "aib100-ota");
	cJSON_AddStringToObject(request, "method", "setConfiguration");
	cJSON_AddItemToObject(request, "params", cJSON_Duplicate(data, 1));
	char* request_text = cJSON_PrintUnformatted(request);
	cJSON_Delete(request);
	char* response_text = request_text ? ACAP_VAPIX_Post("/local/objectanalytics/control.cgi", request_text) : NULL;
	free(request_text);
	cJSON* response = response_text ? cJSON_Parse(response_text) : NULL;
	free(response_text);
	int success = response && !cJSON_GetObjectItem(response, "error");
	cJSON_Delete(response);
	return success;
}

static int Update_Scene_Settings(cJSON* settings, SceneDomain domain,
	const char* scene_name, const uint8_t* fields) {
	cJSON* settings_scene = Settings_Scene(settings, domain, scene_name, 1);
	if (!settings_scene) return 0;
	cJSON* classes = Classes_From_Mask(fields[domain == SCENE_DOMAIN_COUNTING ? 1 : 0]);
	if (!classes) return 0;
	if (cJSON_GetObjectItem(settings_scene, "classes")) cJSON_ReplaceItemInObject(settings_scene, "classes", classes);
	else cJSON_AddItemToObject(settings_scene, "classes", classes);
	if (domain == SCENE_DOMAIN_OCCUPANCY) {
		static const char* values[] = {"max", "min", "average"};
		cJSON* value = cJSON_CreateString(values[fields[1]]);
		if (!value) return 0;
		if (cJSON_GetObjectItem(settings_scene, "value")) cJSON_ReplaceItemInObject(settings_scene, "value", value);
		else cJSON_AddItemToObject(settings_scene, "value", value);
	}
	return 1;
}

static OTA_Status Apply_Pending(SceneDomain domain, PendingSceneUpdate* pending,
	cJSON* settings, cJSON* data, SceneMap* map) {
	if (pending->scene_index < 1 || pending->scene_index > map->count) return OTA_STATUS_UNKNOWN_SCENE;
	SceneEntry* entry = &map->entries[pending->scene_index - 1];
	if ((uint16_t)entry->id != pending->scene_id || entry->fingerprint != pending->scene_fingerprint)
		return OTA_STATUS_SCENE_FINGERPRINT_MISMATCH;
	if (map->fingerprint != pending->map_fingerprint) return OTA_STATUS_MAP_FINGERPRINT_MISMATCH;
	cJSON* original_data = cJSON_Duplicate(data, 1);
	cJSON* candidate_settings = NULL;
	cJSON* live_transmission = NULL;
	if (!original_data) return OTA_STATUS_APPLY_FAILED;
	if (domain != SCENE_DOMAIN_PRESENCE) {
		candidate_settings = cJSON_Duplicate(settings, 1);
		if (!candidate_settings || !Update_Scene_Settings(candidate_settings, domain, entry->name, pending->fields)) {
			cJSON_Delete(original_data);
			cJSON_Delete(candidate_settings);
			return OTA_STATUS_APPLY_FAILED;
		}
		cJSON* candidate_transmission = cJSON_GetObjectItem(candidate_settings, "transmission");
		live_transmission = candidate_transmission ? cJSON_Duplicate(candidate_transmission, 1) : NULL;
		if (!live_transmission) {
			cJSON_Delete(original_data);
			cJSON_Delete(candidate_settings);
			return OTA_STATUS_APPLY_FAILED;
		}
	}
	if (domain == SCENE_DOMAIN_PRESENCE) {
		candidate_settings = cJSON_Duplicate(settings, 1);
		if (!candidate_settings) {
			cJSON_Delete(original_data);
			return OTA_STATUS_APPLY_FAILED;
		}
		Update_Presence_Schedule_Settings(candidate_settings, entry->name, pending->fields);
	}
	if (!Replace_Vertices(entry->json, domain, pending)) {
		cJSON_Delete(original_data);
		cJSON_Delete(candidate_settings);
		cJSON_Delete(live_transmission);
		return OTA_STATUS_APPLY_FAILED;
	}

	if (domain == SCENE_DOMAIN_COUNTING) {
		cJSON* trigger = Scene_Trigger(entry->json, domain);
		cJSON* direction = cJSON_CreateString(pending->fields[0] ? "rightToLeft" : "leftToRight");
		if (cJSON_GetObjectItem(trigger, "countingDirection")) cJSON_ReplaceItemInObject(trigger, "countingDirection", direction);
		else cJSON_AddItemToObject(trigger, "countingDirection", direction);
	} else if (domain == SCENE_DOMAIN_PRESENCE) {
		cJSON* classifications = AOA_Classifications_From_Mask(pending->fields[0]);
		if (cJSON_GetObjectItem(entry->json, "objectClassifications")) cJSON_ReplaceItemInObject(entry->json, "objectClassifications", classifications);
		else cJSON_AddItemToObject(entry->json, "objectClassifications", classifications);
		cJSON* threshold = cJSON_CreateObject();
		cJSON_AddBoolToObject(threshold, "enabled", 1);
		cJSON_AddNumberToObject(threshold, "triggerDelay", Read_U16(pending->fields + 2));
		cJSON* thresholds = cJSON_CreateArray();
		cJSON* item = cJSON_CreateObject();
		cJSON_AddNumberToObject(item, "level", pending->fields[1] - 1);
		cJSON_AddStringToObject(item, "type", "moreThan");
		cJSON_AddItemToArray(thresholds, item);
		cJSON_AddItemToObject(threshold, "thresholds", thresholds);
		if (cJSON_GetObjectItem(entry->json, "thresholdConfiguration")) cJSON_ReplaceItemInObject(entry->json, "thresholdConfiguration", threshold);
		else cJSON_AddItemToObject(entry->json, "thresholdConfiguration", threshold);
	}

	if (!Apply_AOA_Configuration(data)) {
		cJSON_Delete(original_data);
		cJSON_Delete(candidate_settings);
		cJSON_Delete(live_transmission);
		return OTA_STATUS_APPLY_FAILED;
	}
	if (domain == SCENE_DOMAIN_PRESENCE) {
		if (!ACAP_FILE_Write("localdata/settings.json", candidate_settings)) {
			Apply_AOA_Configuration(original_data);
			cJSON_Delete(original_data);
			cJSON_Delete(candidate_settings);
			return OTA_STATUS_APPLY_FAILED;
		}
		cJSON* transmission = cJSON_GetObjectItem(candidate_settings, "transmission");
		if (transmission && cJSON_IsObject(transmission)) {
			cJSON* duplicate = cJSON_Duplicate(transmission, 1);
			if (duplicate) cJSON_ReplaceItemInObject(settings, "transmission", duplicate);
			cJSON* live_transmission = cJSON_GetObjectItem(settings, "transmission");
			Presence_Load_Config(cJSON_GetObjectItem(live_transmission, "presence"), 3);
		}
		cJSON_Delete(original_data);
		cJSON_Delete(candidate_settings);
		Presence_Initialize_State();
		return OTA_STATUS_OK;
	}

	if (!ACAP_FILE_Write("localdata/settings.json", candidate_settings)) {
		Apply_AOA_Configuration(original_data);
		cJSON_Delete(original_data);
		cJSON_Delete(candidate_settings);
		cJSON_Delete(live_transmission);
		return OTA_STATUS_APPLY_FAILED;
	}
	cJSON_Delete(original_data);
	cJSON_Delete(candidate_settings);
	cJSON_ReplaceItemInObject(settings, "transmission", live_transmission);
	cJSON* transmission = cJSON_GetObjectItem(settings, "transmission");
	if (domain == SCENE_DOMAIN_COUNTING) {
		Counting_Load_Config(cJSON_GetObjectItem(transmission, "counting"), 1);
		Counting_Reset_Schedule(time(NULL));
	} else {
		Occupancy_Load_Config(cJSON_GetObjectItem(transmission, "occupancy"), 2);
		Occupancy_Reset_Schedule(time(NULL));
		Occupancy_Update_ACAP_Status();
	}
	return OTA_STATUS_OK;
}

static OTA_Status Validate_Set_Page(SceneDomain domain, const OTA_Frame* frame,
	SceneMap* map, PendingSceneUpdate** pending_out) {
	int static_length = Static_Field_Length(domain);
	if (frame->body_length < (size_t)(13 + static_length)) return OTA_STATUS_INVALID_LENGTH;
	const uint8_t* body = frame->body;
	uint8_t config_version = body[0];
	if (config_version != OTA_SCENE_CONFIG_VERSION_Q15 &&
		config_version != OTA_SCENE_CONFIG_VERSION_INTEGER) return OTA_STATUS_UNSUPPORTED;
	if (body[1] < 1 || body[1] > map->count) return OTA_STATUS_UNKNOWN_SCENE;
	SceneEntry* entry = &map->entries[body[1] - 1];
	if (Read_U16(body + 2) != (uint16_t)entry->id || Read_U16(body + 4) != entry->fingerprint)
		return OTA_STATUS_SCENE_FINGERPRINT_MISMATCH;
	if (Read_U16(body + 6) != map->fingerprint) return OTA_STATUS_MAP_FINGERPRINT_MISMATCH;
	int min_points = domain == SCENE_DOMAIN_COUNTING ? 2 : 3;
	if (body[12] < min_points || body[12] > OTA_SCENE_MAX_POINTS) return OTA_STATUS_INVALID_RANGE;
	int coordinate_bytes = Coordinate_Bytes(config_version);
	int points_per_page = Points_Per_Page(domain, config_version);
	int expected_pages = (body[12] + points_per_page - 1) / points_per_page;
	if (domain == SCENE_DOMAIN_PRESENCE) {
		int matched = 0;
		int lengths[] = {Static_Field_Length(domain), Legacy_Static_Field_Length(domain)};
		for (size_t candidate_index = 0; candidate_index < sizeof(lengths) / sizeof(lengths[0]); candidate_index++) {
			int candidate_static_length = lengths[candidate_index];
			int candidate_points_per_page = (OTA_MAX_BODY_SIZE - 13 - candidate_static_length) /
				coordinate_bytes;
			int candidate_pages = (body[12] + candidate_points_per_page - 1) / candidate_points_per_page;
			int candidate_start = body[8] * candidate_points_per_page;
			int candidate_count = body[12] - candidate_start;
			if (candidate_count > candidate_points_per_page) candidate_count = candidate_points_per_page;
			if (body[9] == candidate_pages && body[8] < body[9] && body[10] == candidate_start &&
				body[11] == candidate_count && frame->body_length ==
				(size_t)(13 + candidate_static_length + candidate_count * coordinate_bytes)) {
				static_length = candidate_static_length;
				points_per_page = candidate_points_per_page;
				expected_pages = candidate_pages;
				matched = 1;
				break;
			}
		}
		if (!matched) return OTA_STATUS_INVALID_LENGTH;
	}
	if (body[9] != expected_pages || body[8] >= body[9]) return OTA_STATUS_INVALID_VALUE;
	int expected_start = body[8] * points_per_page;
	int expected_count = body[12] - expected_start;
	if (expected_count > points_per_page) expected_count = points_per_page;
	if (body[10] != expected_start || body[11] != expected_count ||
	    frame->body_length != (size_t)(13 + static_length + expected_count * coordinate_bytes))
		return OTA_STATUS_INVALID_LENGTH;
	if (domain == SCENE_DOMAIN_COUNTING && (body[13] > 1 || body[14] > 0x3F || body[15] != 0))
		return OTA_STATUS_INVALID_VALUE;
	if (domain == SCENE_DOMAIN_OCCUPANCY && (body[13] > 0x3F || body[14] > 2 || body[15] != 0))
		return OTA_STATUS_INVALID_VALUE;
	if (domain == SCENE_DOMAIN_PRESENCE && (body[13] == 0 || body[13] > 0x3F || body[14] == 0))
		return OTA_STATUS_INVALID_VALUE;
	if (domain == SCENE_DOMAIN_PRESENCE && (body[14] > 101 || Read_U16(body + 15) > 7200))
		return OTA_STATUS_INVALID_RANGE;
	if (domain == SCENE_DOMAIN_PRESENCE && static_length == Static_Field_Length(domain)) {
		if (body[17] > 1) return OTA_STATUS_INVALID_VALUE;
		if (Read_U16(body + 18) > 1439 || Read_U16(body + 20) > 1439)
			return OTA_STATUS_INVALID_RANGE;
	}

	PendingSceneUpdate* pending = &g_pending[domain];
	if (pending->active && time(NULL) - pending->updated_at > OTA_SCENE_PENDING_TIMEOUT)
		memset(pending, 0, sizeof(*pending));
	if (!pending->active) {
		pending->active = 1;
		pending->transaction_id = frame->transaction_id;
		pending->config_version = config_version;
		pending->scene_index = body[1];
		pending->scene_id = Read_U16(body + 2);
		pending->scene_fingerprint = Read_U16(body + 4);
		pending->map_fingerprint = Read_U16(body + 6);
		pending->page_count = body[9];
		pending->total_points = body[12];
		memset(pending->fields, 0, sizeof(pending->fields));
		memcpy(pending->fields, body + 13, (size_t)static_length);
		if (domain == SCENE_DOMAIN_PRESENCE && static_length == Legacy_Static_Field_Length(domain))
			Ensure_Presence_Schedule_Defaults(pending->fields);
	} else if (pending->transaction_id != frame->transaction_id ||
		pending->config_version != config_version || pending->scene_index != body[1] ||
	           pending->scene_id != Read_U16(body + 2) || pending->scene_fingerprint != Read_U16(body + 4) ||
	           pending->map_fingerprint != Read_U16(body + 6) || pending->page_count != body[9] ||
	           pending->total_points != body[12] ||
		   memcmp(pending->fields, body + 13, (size_t)static_length) != 0) {
		return OTA_STATUS_INVALID_VALUE;
	}
	if (pending->received_pages & (1u << body[8])) return OTA_STATUS_INVALID_VALUE;
	for (int index = 0; index < expected_count; index++) {
		int offset = 13 + static_length + index * coordinate_bytes;
		int16_t x;
		int16_t y;
		if (config_version == OTA_SCENE_CONFIG_VERSION_INTEGER) {
			uint16_t integer_x;
			uint16_t integer_y;
			if (!Read_Integer_Point(body + offset, &integer_x, &integer_y))
				return OTA_STATUS_INVALID_RANGE;
			x = Integer_Coordinate_To_Q15(integer_x);
			y = Integer_Coordinate_To_Q15(integer_y);
		} else {
			x = (int16_t)Read_U16(body + offset);
			y = (int16_t)Read_U16(body + offset + 2);
			if (x == INT16_MIN || y == INT16_MIN) return OTA_STATUS_INVALID_RANGE;
		}
		pending->points[expected_start + index][0] = x;
		pending->points[expected_start + index][1] = y;
	}
	pending->received_pages |= (uint16_t)(1u << body[8]);
	pending->updated_at = time(NULL);
	*pending_out = pending;
	return OTA_STATUS_OK;
}

static void Send_Error(OTA_Scene_Send send_response, int port, const OTA_Frame* frame, OTA_Status status) {
	uint8_t body[] = {frame->command, (uint8_t)status};
	send_response(port, OTA_COMMAND_ERROR, frame->transaction_id, body, sizeof(body));
}

static void OTA_Scene_Handle_Unlocked(int port, const OTA_Frame* frame, cJSON* settings,
	OTA_Scene_Send send_response) {
	if (!frame || !settings || !send_response) return;
	SceneDomain domain = Port_Domain(port);
	cJSON* data = NULL;
	cJSON* root = Fetch_AOA_Configuration(&data);
	if (!root) {
		Send_Error(send_response, port, frame, OTA_STATUS_APPLY_FAILED);
		return;
	}
	SceneMap map;
	Build_Scene_Map(data, settings, domain, &map);

	if (frame->command == OTA_COMMAND_CAPS) {
		if (frame->body_length != 0) Send_Error(send_response, port, frame, OTA_STATUS_INVALID_LENGTH);
		else {
			uint8_t min_points = domain == SCENE_DOMAIN_COUNTING ? 2 : 3;
			uint8_t body[] = {OTA_SCENE_CONFIG_VERSION_INTEGER, OTA_SCENE_MAX_COUNT,
				OTA_MAX_BODY_SIZE, 1, 6, min_points, OTA_SCENE_MAX_POINTS};
			send_response(port, OTA_COMMAND_CAPS_RESPONSE, frame->transaction_id, body, sizeof(body));
		}
		cJSON_Delete(root);
		return;
	}

	if (frame->command == OTA_COMMAND_LIST) {
		if (frame->body_length != 1) Send_Error(send_response, port, frame, OTA_STATUS_INVALID_LENGTH);
		else {
			int entries_per_page = 6;
			int page_count = map.count == 0 ? 1 : (map.count + entries_per_page - 1) / entries_per_page;
			int page = frame->body[0];
			if (page >= page_count) Send_Error(send_response, port, frame, OTA_STATUS_INVALID_RANGE);
			else {
				int start = page * entries_per_page;
				int count = map.count - start;
				if (count > entries_per_page) count = entries_per_page;
				uint8_t body[OTA_MAX_BODY_SIZE];
				Write_U16(body, map.fingerprint);
				body[2] = (uint8_t)map.count;
				body[3] = (uint8_t)page;
				body[4] = (uint8_t)page_count;
				body[5] = (uint8_t)count;
				for (int index = 0; index < count; index++) {
					SceneEntry* entry = &map.entries[start + index];
					int offset = 6 + index * 6;
					body[offset] = (uint8_t)(start + index + 1);
					Write_U16(body + offset + 1, (uint16_t)entry->id);
					Write_U16(body + offset + 3, entry->fingerprint);
					body[offset + 5] = (uint8_t)entry->point_count;
				}
				send_response(port, OTA_COMMAND_LIST_RESPONSE, frame->transaction_id, body, (size_t)(6 + count * 6));
			}
		}
		cJSON_Delete(root);
		return;
	}

	if (frame->command == OTA_COMMAND_GET) {
		if (frame->body_length != 2) Send_Error(send_response, port, frame, OTA_STATUS_INVALID_LENGTH);
		else {
			uint8_t body[OTA_MAX_BODY_SIZE];
			size_t body_length = 0;
			OTA_Status status = Build_Config_Page(domain, &map, frame->body[0], frame->body[1], settings, body, &body_length);
			if (status == OTA_STATUS_OK) send_response(port, OTA_COMMAND_GET_RESPONSE, frame->transaction_id, body, body_length);
			else Send_Error(send_response, port, frame, status);
		}
		cJSON_Delete(root);
		return;
	}

	if (frame->command == OTA_COMMAND_SET) {
		PendingSceneUpdate* pending = NULL;
		OTA_Status status = Validate_Set_Page(domain, frame, &map, &pending);
		if (status == OTA_STATUS_OK) {
			uint16_t complete_mask = (uint16_t)((1u << pending->page_count) - 1u);
			if (pending->received_pages != complete_mask) status = OTA_STATUS_PARTIAL_PAGE_PENDING;
			else status = Apply_Pending(domain, pending, settings, data, &map);
		}
		uint8_t scene_index = frame->body_length > 1 ? frame->body[1] : 0;
		uint8_t body[] = {scene_index, (uint8_t)status};
		send_response(port, OTA_COMMAND_SET_ACK, frame->transaction_id, body, sizeof(body));
		if (status != OTA_STATUS_PARTIAL_PAGE_PENDING && pending) memset(pending, 0, sizeof(*pending));
		cJSON_Delete(root);
		return;
	}

	Send_Error(send_response, port, frame, OTA_STATUS_UNKNOWN_COMMAND);
	cJSON_Delete(root);
}

void OTA_Scene_Handle(int port, const OTA_Frame* frame, cJSON* settings,
	OTA_Scene_Send send_response) {
	pthread_mutex_lock(&g_scene_ota_mutex);
	OTA_Scene_Handle_Unlocked(port, frame, settings, send_response);
	pthread_mutex_unlock(&g_scene_ota_mutex);
}

cJSON* OTA_Scene_Maps_JSON(cJSON* settings) {
	pthread_mutex_lock(&g_scene_ota_mutex);
	cJSON* result = cJSON_CreateObject();
	cJSON* data = NULL;
	cJSON* root = Fetch_AOA_Configuration(&data);
	if (!result || !root) {
		cJSON_Delete(result);
		cJSON_Delete(root);
		pthread_mutex_unlock(&g_scene_ota_mutex);
		return NULL;
	}
	for (int domain = SCENE_DOMAIN_COUNTING; domain <= SCENE_DOMAIN_PRESENCE; domain++) {
		SceneMap map;
		Build_Scene_Map(data, settings, (SceneDomain)domain, &map);
		cJSON* group = cJSON_CreateObject();
		cJSON* scenes = cJSON_CreateArray();
		cJSON_AddNumberToObject(group, "mapFingerprint", map.fingerprint);
		for (int index = 0; index < map.count; index++) {
			SceneEntry* entry = &map.entries[index];
			cJSON* item = cJSON_CreateObject();
			cJSON_AddNumberToObject(item, "index", index + 1);
			cJSON_AddNumberToObject(item, "id", entry->id);
			cJSON_AddStringToObject(item, "name", entry->name);
			cJSON_AddNumberToObject(item, "fingerprint", entry->fingerprint);
			cJSON_AddNumberToObject(item, "pointCount", entry->point_count);
			cJSON_AddItemToArray(scenes, item);
		}
		cJSON_AddItemToObject(group, "scenes", scenes);
		const char* port = domain == SCENE_DOMAIN_COUNTING ? "131" : domain == SCENE_DOMAIN_OCCUPANCY ? "132" : "133";
		cJSON_AddItemToObject(result, port, group);
	}
	cJSON_Delete(root);
	pthread_mutex_unlock(&g_scene_ota_mutex);
	return result;
}