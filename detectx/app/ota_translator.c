#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#include "Model.h"
#include "cJSON.h"
#include "ota_translator.h"

static const char* DETECTX_OTA_ENCODER_JS =
"var DETECTX_OTA_SERVICES = {\n"
"  actions: { port: 100 },\n"
"  configuration: { port: 110 },\n"
"  information: { port: 120 }\n"
"};\n"
"\n"
"function normalize(value) {\n"
"  return String(value == null ? '' : value).replace(/[ _-]/g, '').toLowerCase();\n"
"}\n"
"\n"
"function integer(value, minimum, maximum, name) {\n"
"  var number = Number(value);\n"
"  if (!Number.isInteger(number) || number < minimum || number > maximum) {\n"
"    throw new Error(name + ' must be an integer from ' + minimum + ' through ' + maximum);\n"
"  }\n"
"  return number;\n"
"}\n"
"\n"
"function bytesToHex(bytes) {\n"
"  return bytes.map(function(byte) { return ('0' + byte.toString(16)).slice(-2); }).join('').toUpperCase();\n"
"}\n"
"\n"
"function encodeActions(message, config) {\n"
"  var actions = { restartbridge: 1, joinnetwork: 2, publishoccupancy: 3, publishnow: 3 };\n"
"  var action = normalize(message.action || message.command || config.action);\n"
"  if (actions[action] == null) throw new Error('Action must be restartBridge, joinNetwork, or publishOccupancy');\n"
"  return [actions[action]];\n"
"}\n"
"\n"
"function encodeConfiguration(message, config) {\n"
"  var setting = normalize(message.setting || message.command || config.setting);\n"
"  var value = message.value;\n"
"  if (!setting) {\n"
"    if (config.intervalMinutes != null) { setting = 'intervalminutes'; value = config.intervalMinutes; }\n"
"    else if (config.dataRate != null) { setting = 'datarate'; value = config.dataRate; }\n"
"    else if (config.adrEnabled != null || config.adr != null) { setting = 'adr'; value = config.adrEnabled != null ? config.adrEnabled : config.adr; }\n"
"  }\n"
"  if (value == null && config.value != null) value = config.value;\n"
"  if (setting === 'interval' || setting === 'intervalminutes') return [1, integer(value, 1, 60, 'intervalMinutes')];\n"
"  if (setting === 'datarate' || setting === 'dr') return [2, integer(value, 0, 5, 'dataRate')];\n"
"  if (setting === 'adr' || setting === 'adrenabled') return [3, value ? 1 : 0];\n"
"  throw new Error('Configuration setting must be intervalMinutes, dataRate, or adrEnabled');\n"
"}\n"
"\n"
"function encodeInformation(message, config) {\n"
"  var requests = { camera: 1, camerainfo: 1, bridge: 2, bridgeinfo: 2, link: 3, linkstatus: 3 };\n"
"  var request = normalize(message.request || message.command || message.infoType || config.request || config.service);\n"
"  if (requests[request] == null) throw new Error('Information request must be camera, bridge, or linkStatus');\n"
"  return [requests[request]];\n"
"}\n"
"\n"
"var DETECTX_OTA_ENCODERS = {\n"
"  actions: encodeActions,\n"
"  configuration: encodeConfiguration,\n"
"  information: encodeInformation\n"
"};\n"
"\n"
"function Encode(input) {\n"
"  var message = input && input.data ? input.data : (input || {});\n"
"  var config = message.config || {};\n"
"  var aliases = { action: 'actions', control: 'actions', config: 'configuration', lorawan: 'configuration', info: 'information' };\n"
"  var service = normalize(message.service || message.type);\n"
"  service = aliases[service] || service;\n"
"  if (!DETECTX_OTA_SERVICES[service]) throw new Error('Service must be actions, configuration, or information');\n"
"  var bytes = DETECTX_OTA_ENCODERS[service](message, config);\n"
"  return { port: DETECTX_OTA_SERVICES[service].port, message: bytesToHex(bytes), configuration: DETECTX_CONFIGURATION };\n"
"}\n"
"\n"
"function encodeDownlink(input) {\n"
"  try {\n"
"    var encoded = Encode(input);\n"
"    return { fPort: encoded.port, bytes: encoded.message.match(/../g).map(function(value) { return parseInt(value, 16); }) };\n"
"  } catch (error) {\n"
"    return { errors: [error.message] };\n"
"  }\n"
"}\n";

static const char* DETECTX_OTA_DECODER_JS =
"var DETECTX_OTA_SERVICES = {\n"
"  2: 'occupancy', 100: 'actions', 110: 'configuration', 120: 'information',\n"
"  121: 'cameraInformation', 122: 'bridgeInformation'\n"
"};\n"
"\n"
"function payloadBytes(payload) {\n"
"  if (Array.isArray(payload)) return payload.slice();\n"
"  if (payload instanceof Uint8Array) return Array.prototype.slice.call(payload);\n"
"  var hex = String(payload == null ? '' : payload).replace(/\\s/g, '');\n"
"  if (!/^(?:[0-9a-fA-F]{2})*$/.test(hex)) throw new Error('Payload must be bytes or an even-length hex string');\n"
"  return (hex.match(/../g) || []).map(function(value) { return parseInt(value, 16); });\n"
"}\n"
"\n"
"function ascii(bytes) {\n"
"  return bytes.map(function(byte) { return String.fromCharCode(byte); }).join('');\n"
"}\n"
"\n"
"function decodeOccupancy(bytes) {\n"
"  if (bytes.length !== DETECTX_LABELS.length) throw new Error('Expected ' + DETECTX_LABELS.length + ' occupancy bytes');\n"
"  var occupancy = {};\n"
"  for (var index = 0; index < bytes.length; index++) occupancy[DETECTX_LABELS[index]] = bytes[index];\n"
"  return { port: 2, service: 'occupancy', configuration: DETECTX_CONFIGURATION, occupancy: occupancy };\n"
"}\n"
"\n"
"function decodeCommand(port, bytes) {\n"
"  if (!bytes.length) throw new Error('Command payload is empty');\n"
"  if (port === 100) {\n"
"    var actions = { 1: 'restartBridge', 2: 'joinNetwork', 3: 'publishOccupancy' };\n"
"    if (!actions[bytes[0]]) throw new Error('Unknown action command');\n"
"    return { port: port, service: 'actions', action: actions[bytes[0]] };\n"
"  }\n"
"  if (port === 110) {\n"
"    if (bytes.length < 2) throw new Error('Configuration command requires a value');\n"
"    var settings = { 1: 'intervalMinutes', 2: 'dataRate', 3: 'adrEnabled' };\n"
"    if (!settings[bytes[0]]) throw new Error('Unknown configuration command');\n"
"    var value = bytes[0] === 3 ? bytes[1] !== 0 : bytes[1];\n"
"    return { port: port, service: 'configuration', setting: settings[bytes[0]], value: value };\n"
"  }\n"
"  var requests = { 1: 'camera', 2: 'bridge', 3: 'linkStatus' };\n"
"  if (!requests[bytes[0]]) throw new Error('Unknown information command');\n"
"  return { port: port, service: 'information', request: requests[bytes[0]] };\n"
"}\n"
"\n"
"function decodeCameraInformation(bytes) {\n"
"  var fields = ascii(bytes).split(',');\n"
"  return { port: 121, service: 'cameraInformation', model: fields[0] || '', serial: fields[1] || '',\n"
"    firmware: fields[2] || '', uptimeHours: Number(String(fields[3] || '').replace(/h$/, '')), appVersion: fields[4] || '' };\n"
"}\n"
"\n"
"function decodeBridgeInformation(bytes) {\n"
"  var fields = ascii(bytes).split(',');\n"
"  var hardware = String(fields[0] || '').split('/');\n"
"  return { port: 122, service: 'bridgeInformation', hardware: hardware[0] || '', hardwareVersion: hardware[1] || '',\n"
"    firmware: fields[1] || '', powerSource: fields[2] || '', temperatureC: Number(String(fields[3] || '').replace(/C$/, '')),\n"
"    restartCounter: Number(String(fields[4] || '').replace(/^R/, '')), devAddr: fields[5] || '' };\n"
"}\n"
"\n"
"var DETECTX_OTA_DECODERS = {\n"
"  2: decodeOccupancy,\n"
"  100: function(bytes) { return decodeCommand(100, bytes); },\n"
"  110: function(bytes) { return decodeCommand(110, bytes); },\n"
"  120: function(bytes) { return decodeCommand(120, bytes); },\n"
"  121: decodeCameraInformation,\n"
"  122: decodeBridgeInformation\n"
"};\n"
"\n"
"function Decode(port, payload) {\n"
"  if (typeof port === 'object') { payload = port.bytes != null ? port.bytes : port.message; port = port.fPort != null ? port.fPort : port.port; }\n"
"  port = Number(port);\n"
"  if (!DETECTX_OTA_DECODERS[port]) throw new Error('Unsupported DetectX port ' + port);\n"
"  return DETECTX_OTA_DECODERS[port](payloadBytes(payload));\n"
"}\n"
"\n"
"function decodeUplink(input) {\n"
"  try { return { data: Decode(input) }; }\n"
"  catch (error) { return { errors: [error.message] }; }\n"
"}\n";

static void Build_Label_Metadata(char** labels_text, char** mapping_text) {
	cJSON* labels = cJSON_CreateArray();
	cJSON* mapping = cJSON_CreateArray();
	cJSON* settings = ACAP_Get_Config("settings");
	cJSON* transmission = settings ? cJSON_GetObjectItem(settings, "transmission") : NULL;
	cJSON* occupancy = transmission ? cJSON_GetObjectItem(transmission, "occupancy") : NULL;
	cJSON* selected = occupancy ? cJSON_GetObjectItem(occupancy, "selectedLabels") : NULL;
	int count = cJSON_IsArray(selected) ? cJSON_GetArraySize(selected) : 0;
	if (count > 5) count = 5;
	for (int index = 0; index < count; index++) {
		cJSON* label = cJSON_GetArrayItem(selected, index);
		if (!cJSON_IsString(label) || !label->valuestring[0]) continue;
		cJSON_AddItemToArray(labels, cJSON_CreateString(label->valuestring));
		cJSON* entry = cJSON_CreateObject();
		cJSON_AddNumberToObject(entry, "byte", cJSON_GetArraySize(mapping));
		cJSON_AddStringToObject(entry, "label", label->valuestring);
		cJSON_AddItemToArray(mapping, entry);
	}
	*labels_text = cJSON_PrintUnformatted(labels);
	*mapping_text = cJSON_PrintUnformatted(mapping);
	cJSON_Delete(labels);
	cJSON_Delete(mapping);
}

static void Append_Label_Metadata(GString* javascript) {
	char* labels_text = NULL;
	char* mapping_text = NULL;
	Build_Label_Metadata(&labels_text, &mapping_text);
	g_string_append_printf(javascript,
		"var DETECTX_LABELS = %s;\n"
		"var DETECTX_LABEL_MAPPING = %s;\n"
		"var DETECTX_CONFIGURATION = { occupancy: { port: 2, labels: DETECTX_LABEL_MAPPING } };\n\n",
		labels_text ? labels_text : "[]", mapping_text ? mapping_text : "[]");
	free(labels_text);
	free(mapping_text);
}

void OTA_Translator_Data(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	(void)request;
	GString* javascript = g_string_sized_new(4096);
	const cJSON* model = Model_Get_Config();
	const cJSON* hash = model ? cJSON_GetObjectItem(model, "modelSha256") : NULL;
	g_string_append_printf(javascript, "// AI-B100 DetectX 2.0.0 | model SHA-256: %s\n",
		cJSON_IsString(hash) ? hash->valuestring : "unknown");
	Append_Label_Metadata(javascript);
	g_string_append(javascript,
		"function decodeUplink(input) {\n"
		"  if (input.fPort !== 2) return { errors: ['AI-B100 DetectX expects port 2'] };\n"
		"  if (input.bytes.length !== DETECTX_LABELS.length) return { errors: ['Expected ' + DETECTX_LABELS.length + ' bytes'] };\n"
		"  var occupancy = {};\n"
		"  for (var index = 0; index < input.bytes.length; index++) occupancy[DETECTX_LABELS[index]] = input.bytes[index];\n"
		"  return { data: { port: 2, useCase: 'occupancy', configuration: DETECTX_CONFIGURATION, occupancy: occupancy } };\n"
		"}\n");
	ACAP_HTTP_Header_FILE(response, "Decoder-AI-B100-DetectX.js", "application/javascript", javascript->len);
	ACAP_HTTP_Respond_Data(response, javascript->len, javascript->str);
	g_string_free(javascript, TRUE);
}

static void Respond_Translator(ACAP_HTTP_Response response, int encoder) {
	GString* javascript = g_string_sized_new(8192);
	g_string_append_printf(javascript, "/** AI-B100 DetectX OTA %s. */\n", encoder ? "Encoder" : "Decoder");
	Append_Label_Metadata(javascript);
	g_string_append(javascript, encoder ? DETECTX_OTA_ENCODER_JS : DETECTX_OTA_DECODER_JS);
	char filename[96];
	snprintf(filename, sizeof(filename), "aib100-detectx-ota-%s-%s.js",
		encoder ? "encoder" : "decoder", ACAP_DEVICE_Date());
	ACAP_HTTP_Header_FILE(response, filename, "application/javascript", javascript->len);
	ACAP_HTTP_Respond_Data(response, javascript->len, javascript->str);
	g_string_free(javascript, TRUE);
}

void OTA_Translator_Encoder(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	(void)request;
	Respond_Translator(response, 1);
}

void OTA_Translator_Decoder(ACAP_HTTP_Response response, ACAP_HTTP_Request request) {
	(void)request;
	Respond_Translator(response, 0);
}