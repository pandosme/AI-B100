#ifndef DETECTX_OTA_TRANSLATOR_H
#define DETECTX_OTA_TRANSLATOR_H

#include "ACAP.h"

void OTA_Translator_Data(ACAP_HTTP_Response response, ACAP_HTTP_Request request);
void OTA_Translator_Encoder(ACAP_HTTP_Response response, ACAP_HTTP_Request request);
void OTA_Translator_Decoder(ACAP_HTTP_Response response, ACAP_HTTP_Request request);

#endif