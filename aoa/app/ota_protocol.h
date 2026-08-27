#ifndef OTA_PROTOCOL_H
#define OTA_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define OTA_PROTOCOL_VERSION 1
#define OTA_MAX_FRAME_SIZE 51
#define OTA_FRAME_OVERHEAD 4
#define OTA_MAX_BODY_SIZE (OTA_MAX_FRAME_SIZE - OTA_FRAME_OVERHEAD)

typedef enum {
	OTA_COMMAND_GET = 0x01,
	OTA_COMMAND_SET = 0x02,
	OTA_COMMAND_CAPS = 0x03,
	OTA_COMMAND_LIST = 0x04,
	OTA_COMMAND_GET_RESPONSE = 0x81,
	OTA_COMMAND_SET_ACK = 0x82,
	OTA_COMMAND_CAPS_RESPONSE = 0x83,
	OTA_COMMAND_LIST_RESPONSE = 0x84,
	OTA_COMMAND_ERROR = 0xE0
} OTA_Command;

typedef enum {
	OTA_STATUS_OK = 0x00,
	OTA_STATUS_INVALID_LENGTH = 0x01,
	OTA_STATUS_INVALID_VALUE = 0x02,
	OTA_STATUS_INVALID_RANGE = 0x03,
	OTA_STATUS_CRC_MISMATCH = 0x04,
	OTA_STATUS_UNKNOWN_COMMAND = 0x05,
	OTA_STATUS_UNKNOWN_SCENE = 0x06,
	OTA_STATUS_SCENE_FINGERPRINT_MISMATCH = 0x07,
	OTA_STATUS_MAP_FINGERPRINT_MISMATCH = 0x08,
	OTA_STATUS_PARTIAL_PAGE_PENDING = 0x09,
	OTA_STATUS_APPLY_FAILED = 0x0A,
	OTA_STATUS_UNSUPPORTED = 0x0B
} OTA_Status;

typedef struct {
	uint8_t command;
	uint8_t version;
	uint8_t transaction_id;
	const uint8_t* body;
	size_t body_length;
} OTA_Frame;

uint8_t OTA_CRC8(const uint8_t* data, size_t length);
OTA_Status OTA_Decode_Frame(const uint8_t* data, size_t length, OTA_Frame* frame);
OTA_Status OTA_Encode_Frame(uint8_t command, uint8_t transaction_id,
	const uint8_t* body, size_t body_length, uint8_t* output,
	size_t output_capacity, size_t* output_length);

#endif