#ifndef OTA_COUNTING_H
#define OTA_COUNTING_H

#include "cJSON.h"
#include "ota_protocol.h"

typedef int (*OTA_Counting_Send)(int port, uint8_t command, uint8_t transaction_id,
	const uint8_t* body, size_t body_length);
typedef int (*OTA_Counting_Persist)(const char* path, cJSON* settings);
typedef void (*OTA_Counting_Apply)(const char* service, cJSON* data);

void OTA_Counting_Handle(const OTA_Frame* frame, cJSON* settings,
	OTA_Counting_Send send_response, OTA_Counting_Persist persist_settings,
	OTA_Counting_Apply apply_settings);

#endif
