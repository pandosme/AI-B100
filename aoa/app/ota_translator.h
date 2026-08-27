#ifndef OTA_TRANSLATOR_H
#define OTA_TRANSLATOR_H

#include "ACAP.h"

void HTTP_Endpoint_OTA_Encoder(ACAP_HTTP_Response response, const ACAP_HTTP_Request request);
void HTTP_Endpoint_OTA_Decoder(ACAP_HTTP_Response response, const ACAP_HTTP_Request request);

#endif