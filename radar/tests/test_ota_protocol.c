#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ota_protocol.h"

int main(void) {
	const uint8_t check[] = "123456789";
	assert(OTA_CRC8(check, strlen((const char*)check)) == 0xF4);

	uint8_t body[] = {1, 1, 5};
	uint8_t encoded[OTA_MAX_FRAME_SIZE];
	size_t encoded_length = 0;
	assert(OTA_Encode_Frame(OTA_COMMAND_SET, 42, body, sizeof(body), encoded,
		sizeof(encoded), &encoded_length) == OTA_STATUS_OK);
	assert(encoded_length == 7);

	OTA_Frame decoded;
	assert(OTA_Decode_Frame(encoded, encoded_length, &decoded) == OTA_STATUS_OK);
	assert(decoded.command == OTA_COMMAND_SET);
	assert(decoded.version == OTA_PROTOCOL_VERSION);
	assert(decoded.transaction_id == 42);
	assert(decoded.body_length == sizeof(body));
	assert(memcmp(decoded.body, body, sizeof(body)) == 0);

	encoded[3] ^= 1;
	assert(OTA_Decode_Frame(encoded, encoded_length, &decoded) == OTA_STATUS_CRC_MISMATCH);
	encoded[3] ^= 1;

	uint8_t maximum_body[OTA_MAX_BODY_SIZE] = {0};
	assert(OTA_Encode_Frame(OTA_COMMAND_SET, 0, maximum_body, sizeof(maximum_body),
		encoded, sizeof(encoded), &encoded_length) == OTA_STATUS_OK);
	assert(encoded_length == OTA_MAX_FRAME_SIZE);
	assert(OTA_Encode_Frame(OTA_COMMAND_SET, 0, maximum_body, sizeof(maximum_body) + 1,
		encoded, sizeof(encoded), &encoded_length) == OTA_STATUS_INVALID_LENGTH);
	return 0;
}
