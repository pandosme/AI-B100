#include "ota_protocol.h"

uint8_t OTA_CRC8(const uint8_t* data, size_t length) {
	uint8_t crc = 0;
	for (size_t index = 0; index < length; index++) {
		crc ^= data[index];
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
	}
	return crc;
}

OTA_Status OTA_Decode_Frame(const uint8_t* data, size_t length, OTA_Frame* frame) {
	if (!data || !frame || length < OTA_FRAME_OVERHEAD || length > OTA_MAX_FRAME_SIZE)
		return OTA_STATUS_INVALID_LENGTH;
	if (OTA_CRC8(data, length - 1) != data[length - 1])
		return OTA_STATUS_CRC_MISMATCH;
	if (data[1] != OTA_PROTOCOL_VERSION)
		return OTA_STATUS_UNSUPPORTED;

	frame->command = data[0];
	frame->version = data[1];
	frame->transaction_id = data[2];
	frame->body = data + 3;
	frame->body_length = length - OTA_FRAME_OVERHEAD;
	return OTA_STATUS_OK;
}

OTA_Status OTA_Encode_Frame(uint8_t command, uint8_t transaction_id,
	const uint8_t* body, size_t body_length, uint8_t* output,
	size_t output_capacity, size_t* output_length) {
	size_t frame_length = body_length + OTA_FRAME_OVERHEAD;
	if (!output || !output_length || (body_length > 0 && !body) ||
	    body_length > OTA_MAX_BODY_SIZE || output_capacity < frame_length)
		return OTA_STATUS_INVALID_LENGTH;

	output[0] = command;
	output[1] = OTA_PROTOCOL_VERSION;
	output[2] = transaction_id;
	for (size_t index = 0; index < body_length; index++) output[index + 3] = body[index];
	output[frame_length - 1] = OTA_CRC8(output, frame_length - 1);
	*output_length = frame_length;
	return OTA_STATUS_OK;
}