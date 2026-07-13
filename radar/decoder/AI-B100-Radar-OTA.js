/**
 * AI-B100 Radar OTA Translator
 *
 * Port
 * - Radar OTA uses dedicated port 130 for both requests and responses.
 *
 * Commands
 * - 0x01: GET_CONFIG request
 * - 0x81: GET_CONFIG response
 * - 0x02: SET_CONFIG request
 * - 0x82: SET_CONFIG ACK/NACK
 * - 0x03: GET_CAPS request
 * - 0x83: GET_CAPS response
 *
 * Config body format (8 bytes) for 0x81 and 0x02 body:
 * byte[0]   protocolVersion (uint8)
 * byte[1:2] fieldMask (uint16 BE, bit 0x0001 = detectionSensitivity)
 * byte[3]   detectionSensitivity (1=low, 2=medium, 3=high)
 * byte[4:6] reserved (0)
 * byte[7]   crc8 over bytes[0..6]
 *
 * Public API
 * - Encode_Config(config, command) : JSON -> HEX payload
 * - Decode_config(input)           : HEX/bytes -> JSON
 */

var AI_B100_RADAR_OTA_PORT = 130;
var RADAR_OTA_FIELD_DETECTION_SENSITIVITY = 0x0001;

function aiByte(value) { return value & 255; }

function aiClamp(value, minValue, maxValue) {
  var n = Number(value);
  if (!Number.isFinite(n)) n = minValue;
  if (n < minValue) n = minValue;
  if (n > maxValue) n = maxValue;
  return Math.round(n);
}

function aiHexToBytes(hex) {
  var clean = String(hex || '').replace(/\s+/g, '').toUpperCase();
  if (!clean.length) return [];
  if ((clean.length & 1) !== 0 || !/^[0-9A-F]+$/.test(clean)) throw new Error('HEX must be even-length hexadecimal string');
  var bytes = [];
  for (var i = 0; i < clean.length; i += 2) bytes.push(parseInt(clean.substr(i, 2), 16));
  return bytes;
}

function aiBytesToHex(bytes) {
  return (bytes || []).map(function(value) { return ('0' + aiByte(value).toString(16)).slice(-2); }).join('').toUpperCase();
}

function aiWriteU16BE(out, value) {
  var v = aiClamp(value, 0, 65535);
  out.push((v >> 8) & 255);
  out.push(v & 255);
}

function aiReadU16BE(bytes, index) {
  return (aiByte(bytes[index]) << 8) | aiByte(bytes[index + 1]);
}

function aiCrc8(bytes) {
  var crc = 0;
  for (var i = 0; i < bytes.length; i++) {
    crc ^= aiByte(bytes[i]);
    for (var bit = 0; bit < 8; bit++) {
      if (crc & 128) crc = ((crc << 1) ^ 0x07) & 255;
      else crc = (crc << 1) & 255;
    }
  }
  return crc & 255;
}

function aiSensitivityCode(value) {
  var s = String(value || '').trim().toLowerCase();
  if (s === 'low') return 1;
  if (s === 'medium') return 2;
  if (s === 'high') return 3;
  throw new Error('detectionSensitivity must be low, medium, or high');
}

function aiSensitivityName(code) {
  if (code === 1) return 'low';
  if (code === 2) return 'medium';
  if (code === 3) return 'high';
  return 'unknown';
}

function aiBuildConfigBody(config) {
  var cfg = config || {};
  var body = [];
  body.push(aiClamp(cfg.protocolVersion || 1, 1, 255));
  aiWriteU16BE(body, RADAR_OTA_FIELD_DETECTION_SENSITIVITY);
  body.push(aiSensitivityCode(cfg.detectionSensitivity));
  body.push(0);
  body.push(0);
  body.push(0);
  body.push(aiCrc8(body));
  if (body.length !== 8) throw new Error('Config body must be 8 bytes');
  return body;
}

function aiParseConfigBody(body) {
  if (!body || body.length !== 8) throw new Error('Config body must be 8 bytes');
  if (aiCrc8(body.slice(0, 7)) !== aiByte(body[7])) throw new Error('Config CRC mismatch');
  var fieldMask = aiReadU16BE(body, 1);
  var sensitivityCode = aiByte(body[3]);
  return {
    protocolVersion: aiByte(body[0]),
    fieldMask: fieldMask,
    detectionSensitivity: aiSensitivityName(sensitivityCode),
    fields: { detectionSensitivity: aiSensitivityName(sensitivityCode) },
    reserved: [aiByte(body[4]), aiByte(body[5]), aiByte(body[6])],
    crc8: aiByte(body[7])
  };
}

function Encode_Config(config, command) {
  var cmd = aiByte(command);
  if (cmd === 0x01 || cmd === 0x03) return aiBytesToHex([cmd]);
  if (cmd !== 0x02) throw new Error('Unsupported command for JSON encode');
  return aiBytesToHex([0x02].concat(aiBuildConfigBody(config || {})));
}

function Decode_config(input) {
  var bytes = Array.isArray(input) ? input.slice() : aiHexToBytes(input);
  if (!bytes.length) throw new Error('Empty payload');
  var command = aiByte(bytes[0]);

  if (command === 0x01 || command === 0x03) {
    return {
      port: AI_B100_RADAR_OTA_PORT,
      command: command,
      type: command === 0x01 ? 'get_config_request' : 'get_caps_request'
    };
  }

  if (command === 0x02 || command === 0x81) {
    if (bytes.length !== 9) throw new Error('Config request/response must be 9 bytes');
    return {
      port: AI_B100_RADAR_OTA_PORT,
      command: command,
      type: command === 0x02 ? 'set_config_request' : 'get_config_response',
      config: aiParseConfigBody(bytes.slice(1))
    };
  }

  if (command === 0x82) {
    if (bytes.length !== 5) throw new Error('ACK/NACK must be 5 bytes');
    if (aiCrc8(bytes.slice(0, 4)) !== aiByte(bytes[4])) throw new Error('ACK/NACK CRC mismatch');
    return {
      port: AI_B100_RADAR_OTA_PORT,
      command: command,
      type: 'set_config_ack',
      version: aiByte(bytes[1]),
      ackFor: aiByte(bytes[2]),
      status: aiByte(bytes[3]),
      success: aiByte(bytes[3]) === 0
    };
  }

  if (command === 0x83) {
    if (bytes.length !== 8) throw new Error('Capabilities response must be 8 bytes');
    var payload = bytes.slice(1);
    if (aiCrc8(payload.slice(0, 6)) !== aiByte(payload[6])) throw new Error('Capabilities CRC mismatch');
    return {
      port: AI_B100_RADAR_OTA_PORT,
      command: command,
      type: 'get_caps_response',
      protocolVersion: aiByte(payload[0]),
      fields: [{ id: aiReadU16BE(payload, 1), name: 'detectionSensitivity', values: ['low', 'medium', 'high'] }],
      detectionSensitivity: { minCode: aiByte(payload[3]), maxCode: aiByte(payload[4]), values: ['low', 'medium', 'high'] }
    };
  }

  throw new Error('Unsupported command byte: 0x' + command.toString(16).toUpperCase());
}

var aiB100RadarOtaJsonToHex = Encode_Config;
var aiB100RadarOtaBufferToJson = Decode_config;