#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "counting.h"
#include "ota_counting.h"

#define COUNTING_OTA_PORT 131
#define COUNTING_OTA_CONFIG_VERSION 1
#define COUNTING_OTA_DELETE 0x80
#define COUNTING_OTA_LIST_PER_PAGE 6

static uint16_t Read_U16(const uint8_t* data) {
	return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

static void Write_U16(uint8_t* data, uint16_t value) {
	data[0] = (uint8_t)value;
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

static uint16_t Scene_Fingerprint(const CountingSceneConfig* scene) {
	uint16_t crc = 0xFFFF;
	crc = CRC16_Update(crc, (uint8_t)scene->id);
	crc = CRC16_Update(crc, (uint8_t)(scene->id >> 8));
	crc = CRC16_String(crc, "radar-crosslinecounting");
	crc = CRC16_String(crc, scene->name);
	crc = CRC16_Update(crc, (uint8_t)scene->enabled);
	crc = CRC16_Update(crc, (uint8_t)scene->direction);
	crc = CRC16_Update(crc, scene->class_mask);
	const int values[] = {scene->x1, scene->y1, scene->x2, scene->y2};
	for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++) {
		crc = CRC16_Update(crc, (uint8_t)values[index]);
		crc = CRC16_Update(crc, (uint8_t)(values[index] >> 8));
	}
	return crc;
}

static uint16_t Map_Fingerprint(const CountingSceneSnapshot* scenes, size_t count) {
	uint16_t crc = 0xFFFF;
	for (size_t index = 0; index < count; index++) {
		uint16_t fingerprint = Scene_Fingerprint(&scenes[index].config);
		crc = CRC16_Update(crc, (uint8_t)(index + 1));
		crc = CRC16_Update(crc, (uint8_t)scenes[index].config.id);
		crc = CRC16_Update(crc, (uint8_t)(scenes[index].config.id >> 8));
		crc = CRC16_Update(crc, (uint8_t)fingerprint);
		crc = CRC16_Update(crc, (uint8_t)(fingerprint >> 8));
	}
	return crc;
}

static void Write_Point(uint8_t* data, int x, int y) {
	uint32_t packed = (uint32_t)x | ((uint32_t)y << 10);
	data[0] = (uint8_t)packed;
	data[1] = (uint8_t)(packed >> 8);
	data[2] = (uint8_t)(packed >> 16);
}

static int Read_Point(const uint8_t* data, int* x, int* y) {
	if (data[2] & 0xF0) return 0;
	uint32_t packed = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16);
	*x = (int)(packed & 0x3FF);
	*y = (int)((packed >> 10) & 0x3FF);
	return *x <= 1000 && *y <= 1000;
}

static uint8_t Scene_Flags(const CountingSceneConfig* scene) {
	return (scene->enabled ? 0x01 : 0) |
		(scene->direction == COUNTING_DIRECTION_RIGHT_TO_LEFT ? 0x02 : 0) |
		(scene->class_mask & COUNTING_CLASS_HUMAN ? 0x04 : 0) |
		(scene->class_mask & COUNTING_CLASS_VEHICLE ? 0x08 : 0);
}

static cJSON* Counting_Scenes_JSON(cJSON* settings) {
	cJSON* transmission = settings ? cJSON_GetObjectItem(settings, "transmission") : NULL;
	cJSON* counting = transmission ? cJSON_GetObjectItem(transmission, "counting") : NULL;
	cJSON* scenes = counting ? cJSON_GetObjectItem(counting, "scenes") : NULL;
	return scenes && cJSON_IsArray(scenes) ? scenes : NULL;
}

static int Find_Scene(const CountingSceneSnapshot* scenes, size_t count, uint16_t id) {
	for (size_t index = 0; index < count; index++)
		if (scenes[index].config.id == id) return (int)index;
	return -1;
}

static uint16_t Allocate_Id(const CountingSceneSnapshot* scenes, size_t count) {
	for (uint32_t id = 1; id <= UINT16_MAX; id++)
		if (Find_Scene(scenes, count, (uint16_t)id) < 0) return (uint16_t)id;
	return 0;
}

static void Send_Error(const OTA_Frame* frame, OTA_Counting_Send send_response, OTA_Status status) {
	uint8_t body[] = {frame->command, (uint8_t)status};
	send_response(COUNTING_OTA_PORT, OTA_COMMAND_ERROR, frame->transaction_id, body, sizeof(body));
}

static OTA_Status Apply_Set(const OTA_Frame* frame, cJSON* settings,
	OTA_Counting_Persist persist_settings, OTA_Counting_Apply apply_settings,
	uint8_t* ack, size_t* ack_length) {
	if (frame->body_length < 16) return OTA_STATUS_INVALID_LENGTH;
	const uint8_t* body = frame->body;
	if (body[0] != COUNTING_OTA_CONFIG_VERSION) return OTA_STATUS_UNSUPPORTED;
	uint8_t scene_index = body[1];
	uint16_t scene_id = Read_U16(body + 2);
	uint16_t scene_fingerprint = Read_U16(body + 4);
	uint16_t map_fingerprint = Read_U16(body + 6);
	uint8_t flags = body[8];
	uint8_t name_length = body[15];
	if (name_length == 0 || name_length >= COUNTING_NAME_SIZE || frame->body_length != 16u + name_length)
		return OTA_STATUS_INVALID_LENGTH;

	CountingSceneSnapshot scenes[COUNTING_MAX_SCENES];
	size_t count = Counting_Get_Scenes(scenes, COUNTING_MAX_SCENES);
	if (map_fingerprint != Map_Fingerprint(scenes, count)) return OTA_STATUS_MAP_FINGERPRINT_MISMATCH;
	int existing = scene_id ? Find_Scene(scenes, count, scene_id) : -1;
	if (flags & COUNTING_OTA_DELETE) {
		if (existing < 0) return OTA_STATUS_UNKNOWN_SCENE;
		if (scene_index != (uint8_t)(existing + 1) || scene_fingerprint != Scene_Fingerprint(&scenes[existing].config))
			return OTA_STATUS_SCENE_FINGERPRINT_MISMATCH;
	} else if (scene_id == 0) {
		if (count >= COUNTING_MAX_SCENES || scene_fingerprint != 0) return OTA_STATUS_INVALID_VALUE;
		scene_id = Allocate_Id(scenes, count);
		if (!scene_id) return OTA_STATUS_APPLY_FAILED;
		scene_index = (uint8_t)(count + 1);
	} else {
		if (existing < 0) return OTA_STATUS_UNKNOWN_SCENE;
		if (scene_index != (uint8_t)(existing + 1) || scene_fingerprint != Scene_Fingerprint(&scenes[existing].config))
			return OTA_STATUS_SCENE_FINGERPRINT_MISMATCH;
	}

	cJSON* candidate = cJSON_Duplicate(settings, 1);
	cJSON* candidate_scenes = Counting_Scenes_JSON(candidate);
	if (!candidate || !candidate_scenes) { cJSON_Delete(candidate); return OTA_STATUS_APPLY_FAILED; }
	if (flags & COUNTING_OTA_DELETE) {
		cJSON_DeleteItemFromArray(candidate_scenes, existing);
	} else {
		CountingSceneConfig config = {0};
		config.id = scene_id;
		config.enabled = (flags & 0x01) != 0;
		config.direction = flags & 0x02 ? COUNTING_DIRECTION_RIGHT_TO_LEFT : COUNTING_DIRECTION_LEFT_TO_RIGHT;
		if (flags & 0x04) config.class_mask |= COUNTING_CLASS_HUMAN;
		if (flags & 0x08) config.class_mask |= COUNTING_CLASS_VEHICLE;
		if (!Read_Point(body + 9, &config.x1, &config.y1) || !Read_Point(body + 12, &config.x2, &config.y2)) {
			cJSON_Delete(candidate); return OTA_STATUS_INVALID_RANGE;
		}
		memcpy(config.name, body + 16, name_length);
		config.name[name_length] = '\0';
		CountingSceneConfig validation[COUNTING_MAX_SCENES];
		for (size_t index = 0; index < count; index++) validation[index] = scenes[index].config;
		if (existing >= 0) validation[existing] = config;
		else validation[count++] = config;
		if (!Counting_Validate_Config(validation, count)) { cJSON_Delete(candidate); return OTA_STATUS_INVALID_VALUE; }

		cJSON* item = cJSON_CreateObject();
		cJSON* classes = cJSON_CreateObject();
		cJSON* line = cJSON_CreateArray();
		cJSON* first = cJSON_CreateObject();
		cJSON* second = cJSON_CreateObject();
		if (!item || !classes || !line || !first || !second) {
			cJSON_Delete(item); cJSON_Delete(classes); cJSON_Delete(line);
			cJSON_Delete(first); cJSON_Delete(second); cJSON_Delete(candidate);
			return OTA_STATUS_APPLY_FAILED;
		}
		cJSON_AddNumberToObject(item, "id", config.id);
		cJSON_AddStringToObject(item, "name", config.name);
		cJSON_AddBoolToObject(item, "enabled", config.enabled);
		cJSON_AddStringToObject(item, "direction", config.direction == COUNTING_DIRECTION_RIGHT_TO_LEFT ? "rightToLeft" : "leftToRight");
		cJSON_AddBoolToObject(classes, "human", (config.class_mask & COUNTING_CLASS_HUMAN) != 0);
		cJSON_AddBoolToObject(classes, "vehicle", (config.class_mask & COUNTING_CLASS_VEHICLE) != 0);
		cJSON_AddItemToObject(item, "classes", classes);
		cJSON_AddNumberToObject(first, "x", config.x1); cJSON_AddNumberToObject(first, "y", config.y1);
		cJSON_AddNumberToObject(second, "x", config.x2); cJSON_AddNumberToObject(second, "y", config.y2);
		cJSON_AddItemToArray(line, first); cJSON_AddItemToArray(line, second); cJSON_AddItemToObject(item, "line", line);
		if (existing >= 0) cJSON_ReplaceItemInArray(candidate_scenes, existing, item);
		else cJSON_AddItemToArray(candidate_scenes, item);
	}

	if (!persist_settings("localdata/settings.json", candidate)) { cJSON_Delete(candidate); return OTA_STATUS_APPLY_FAILED; }
	cJSON* live_transmission = cJSON_GetObjectItem(settings, "transmission");
	cJSON* candidate_transmission = cJSON_GetObjectItem(candidate, "transmission");
	cJSON* replacement = cJSON_Duplicate(candidate_transmission, 1);
	if (!live_transmission || !replacement) { cJSON_Delete(replacement); cJSON_Delete(candidate); return OTA_STATUS_APPLY_FAILED; }
	cJSON_ReplaceItemInObject(settings, "transmission", replacement);
	apply_settings("transmission", replacement);
	cJSON_Delete(candidate);

	CountingSceneSnapshot updated[COUNTING_MAX_SCENES];
	size_t updated_count = Counting_Get_Scenes(updated, COUNTING_MAX_SCENES);
	int updated_index = Find_Scene(updated, updated_count, scene_id);
	ack[0] = OTA_STATUS_OK;
	ack[1] = flags & COUNTING_OTA_DELETE ? scene_index : (uint8_t)(updated_index + 1);
	Write_U16(ack + 2, scene_id);
	Write_U16(ack + 4, updated_index >= 0 ? Scene_Fingerprint(&updated[updated_index].config) : 0);
	Write_U16(ack + 6, Map_Fingerprint(updated, updated_count));
	*ack_length = 8;
	return OTA_STATUS_OK;
}

void OTA_Counting_Handle(const OTA_Frame* frame, cJSON* settings,
	OTA_Counting_Send send_response, OTA_Counting_Persist persist_settings,
	OTA_Counting_Apply apply_settings) {
	if (!frame || !settings || !send_response || !persist_settings || !apply_settings) return;
	CountingSceneSnapshot scenes[COUNTING_MAX_SCENES];
	size_t count = Counting_Get_Scenes(scenes, COUNTING_MAX_SCENES);
	uint16_t map_fingerprint = Map_Fingerprint(scenes, count);
	uint8_t body[OTA_MAX_BODY_SIZE] = {0};
	size_t body_length = 0;

	if (frame->command == OTA_COMMAND_CAPS) {
		if (frame->body_length != 0) return Send_Error(frame, send_response, OTA_STATUS_INVALID_LENGTH);
		uint8_t caps[] = {COUNTING_OTA_CONFIG_VERSION, COUNTING_MAX_SCENES, COUNTING_NAME_SIZE - 1, 2, 0x03};
		send_response(COUNTING_OTA_PORT, OTA_COMMAND_CAPS_RESPONSE, frame->transaction_id, caps, sizeof(caps));
		return;
	}
	if (frame->command == OTA_COMMAND_LIST) {
		if (frame->body_length != 1) return Send_Error(frame, send_response, OTA_STATUS_INVALID_LENGTH);
		uint8_t page_count = (uint8_t)(count ? (count + COUNTING_OTA_LIST_PER_PAGE - 1) / COUNTING_OTA_LIST_PER_PAGE : 1);
		uint8_t page = frame->body[0];
		if (page >= page_count) return Send_Error(frame, send_response, OTA_STATUS_INVALID_RANGE);
		size_t start = page * COUNTING_OTA_LIST_PER_PAGE;
		size_t entries = count - start;
		if (entries > COUNTING_OTA_LIST_PER_PAGE) entries = COUNTING_OTA_LIST_PER_PAGE;
		Write_U16(body, map_fingerprint); body[2] = (uint8_t)count; body[3] = page; body[4] = page_count; body[5] = (uint8_t)entries;
		body_length = 6;
		for (size_t index = 0; index < entries; index++) {
			const CountingSceneConfig* scene = &scenes[start + index].config;
			body[body_length++] = (uint8_t)(start + index + 1);
			Write_U16(body + body_length, scene->id); body_length += 2;
			Write_U16(body + body_length, Scene_Fingerprint(scene)); body_length += 2;
			body[body_length++] = 2;
		}
		send_response(COUNTING_OTA_PORT, OTA_COMMAND_LIST_RESPONSE, frame->transaction_id, body, body_length);
		return;
	}
	if (frame->command == OTA_COMMAND_GET) {
		if (frame->body_length != 2 || frame->body[1] != 0) return Send_Error(frame, send_response, OTA_STATUS_INVALID_LENGTH);
		uint8_t scene_index = frame->body[0];
		if (scene_index == 0 || scene_index > count) return Send_Error(frame, send_response, OTA_STATUS_UNKNOWN_SCENE);
		const CountingSceneConfig* scene = &scenes[scene_index - 1].config;
		size_t name_length = strlen(scene->name);
		body[0] = COUNTING_OTA_CONFIG_VERSION; body[1] = scene_index;
		Write_U16(body + 2, scene->id); Write_U16(body + 4, Scene_Fingerprint(scene)); Write_U16(body + 6, map_fingerprint);
		body[8] = Scene_Flags(scene); Write_Point(body + 9, scene->x1, scene->y1); Write_Point(body + 12, scene->x2, scene->y2);
		body[15] = (uint8_t)name_length; memcpy(body + 16, scene->name, name_length); body_length = 16 + name_length;
		send_response(COUNTING_OTA_PORT, OTA_COMMAND_GET_RESPONSE, frame->transaction_id, body, body_length);
		return;
	}
	if (frame->command == OTA_COMMAND_SET) {
		OTA_Status status = Apply_Set(frame, settings, persist_settings,
			apply_settings, body, &body_length);
		if (status != OTA_STATUS_OK) return Send_Error(frame, send_response, status);
		send_response(COUNTING_OTA_PORT, OTA_COMMAND_SET_ACK, frame->transaction_id, body, body_length);
		return;
	}
	Send_Error(frame, send_response, OTA_STATUS_UNKNOWN_COMMAND);
}
