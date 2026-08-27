#ifndef OTA_SCENE_H
#define OTA_SCENE_H

#include "cJSON.h"
#include "ota_protocol.h"

typedef int (*OTA_Scene_Send)(int port, uint8_t command, uint8_t transaction_id,
	const uint8_t* body, size_t body_length);

void OTA_Scene_Handle(int port, const OTA_Frame* frame, cJSON* settings,
	OTA_Scene_Send send_response);
cJSON* OTA_Scene_Maps_JSON(cJSON* settings);

#endif