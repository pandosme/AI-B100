#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "counting.h"
#include "ota_counting.h"

static uint8_t response_command;
static uint8_t response_body[OTA_MAX_BODY_SIZE];
static size_t response_length;
static cJSON* live_settings;

int ACAP_FILE_Write(const char* path, cJSON* object) {
	assert(strcmp(path, "localdata/settings.json") == 0);
	assert(object != NULL);
	return 1;
}

static int send_response(int port, uint8_t command, uint8_t transaction_id,
	const uint8_t* body, size_t body_length) {
	assert(port == 131);
	assert(transaction_id == 7);
	response_command = command;
	response_length = body_length;
	memcpy(response_body, body, body_length);
	return 1;
}

static void apply_settings(const char* service, cJSON* transmission) {
	assert(strcmp(service, "transmission") == 0);
	cJSON* counting = cJSON_GetObjectItem(transmission, "counting");
	cJSON* scenes_json = cJSON_GetObjectItem(counting, "scenes");
	CountingSceneConfig scenes[COUNTING_MAX_SCENES] = {0};
	int count = cJSON_GetArraySize(scenes_json);
	for (int index = 0; index < count; index++) {
		cJSON* item = cJSON_GetArrayItem(scenes_json, index);
		cJSON* classes = cJSON_GetObjectItem(item, "classes");
		cJSON* line = cJSON_GetObjectItem(item, "line");
		cJSON* first = cJSON_GetArrayItem(line, 0);
		cJSON* second = cJSON_GetArrayItem(line, 1);
		scenes[index].id = (uint16_t)cJSON_GetObjectItem(item, "id")->valueint;
		strncpy(scenes[index].name, cJSON_GetObjectItem(item, "name")->valuestring,
			sizeof(scenes[index].name) - 1);
		scenes[index].enabled = cJSON_IsTrue(cJSON_GetObjectItem(item, "enabled"));
		scenes[index].direction = strcmp(cJSON_GetObjectItem(item, "direction")->valuestring,
			"rightToLeft") == 0 ? COUNTING_DIRECTION_RIGHT_TO_LEFT : COUNTING_DIRECTION_LEFT_TO_RIGHT;
		if (cJSON_IsTrue(cJSON_GetObjectItem(classes, "human"))) scenes[index].class_mask |= COUNTING_CLASS_HUMAN;
		if (cJSON_IsTrue(cJSON_GetObjectItem(classes, "vehicle"))) scenes[index].class_mask |= COUNTING_CLASS_VEHICLE;
		scenes[index].x1 = cJSON_GetObjectItem(first, "x")->valueint;
		scenes[index].y1 = cJSON_GetObjectItem(first, "y")->valueint;
		scenes[index].x2 = cJSON_GetObjectItem(second, "x")->valueint;
		scenes[index].y2 = cJSON_GetObjectItem(second, "y")->valueint;
	}
	assert(Counting_Configure(scenes, (size_t)count));
}

static cJSON* make_settings(void) {
	return cJSON_Parse("{\"transmission\":{\"counting\":{\"enabled\":true,\"port\":1,\"intervalMinutes\":15,\"scenes\":[{\"id\":1,\"name\":\"Right\",\"enabled\":true,\"direction\":\"leftToRight\",\"classes\":{\"human\":true,\"vehicle\":true},\"line\":[{\"x\":500,\"y\":100},{\"x\":500,\"y\":900}]}]}}}");
}

static void handle(uint8_t command, const uint8_t* body, size_t body_length) {
	OTA_Frame frame = {.command = command, .version = 1, .transaction_id = 7,
		.body = body, .body_length = body_length};
	response_command = 0;
	response_length = 0;
	OTA_Counting_Handle(&frame, live_settings, send_response, ACAP_FILE_Write, apply_settings);
}

int main(void) {
	live_settings = make_settings();
	assert(live_settings);
	apply_settings("transmission", cJSON_GetObjectItem(live_settings, "transmission"));

	handle(OTA_COMMAND_CAPS, NULL, 0);
	assert(response_command == OTA_COMMAND_CAPS_RESPONSE && response_length == 5);
	uint8_t page[] = {0};
	handle(OTA_COMMAND_LIST, page, sizeof(page));
	assert(response_command == OTA_COMMAND_LIST_RESPONSE && response_body[2] == 1 && response_body[5] == 1);
	uint8_t get[] = {1, 0};
	handle(OTA_COMMAND_GET, get, sizeof(get));
	assert(response_command == OTA_COMMAND_GET_RESPONSE && response_body[1] == 1);

	uint8_t update[OTA_MAX_BODY_SIZE];
	size_t update_length = response_length;
	memcpy(update, response_body, update_length);
	update[8] ^= 0x02;
	handle(OTA_COMMAND_SET, update, update_length);
	assert(response_command == OTA_COMMAND_SET_ACK && response_body[0] == OTA_STATUS_OK);
	CountingSceneSnapshot scenes[2];
	assert(Counting_Get_Scenes(scenes, 2) == 1);
	assert(scenes[0].config.direction == COUNTING_DIRECTION_RIGHT_TO_LEFT);

	handle(OTA_COMMAND_GET, get, sizeof(get));
	uint8_t create[OTA_MAX_BODY_SIZE] = {0};
	create[0] = 1;
	create[6] = response_body[6]; create[7] = response_body[7];
	create[8] = 0x0F;
	memcpy(create + 9, response_body + 9, 6);
	create[15] = 4;
	memcpy(create + 16, "Left", 4);
	handle(OTA_COMMAND_SET, create, 20);
	assert(response_command == OTA_COMMAND_SET_ACK && response_body[0] == OTA_STATUS_OK);
	assert(Counting_Get_Scenes(scenes, 2) == 2 && scenes[1].config.id == 2);

	uint8_t get_second[] = {2, 0};
	handle(OTA_COMMAND_GET, get_second, sizeof(get_second));
	uint8_t remove[OTA_MAX_BODY_SIZE];
	size_t remove_length = response_length;
	memcpy(remove, response_body, remove_length);
	remove[8] = 0x80;
	handle(OTA_COMMAND_SET, remove, remove_length);
	assert(response_command == OTA_COMMAND_SET_ACK && Counting_Get_Scenes(scenes, 2) == 1);

	handle(OTA_COMMAND_SET, remove, remove_length);
	assert(response_command == OTA_COMMAND_ERROR && response_body[1] == OTA_STATUS_MAP_FINGERPRINT_MISMATCH);
	cJSON_Delete(live_settings);
	return 0;
}
