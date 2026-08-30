/**
 * AI-B100 Detection Alert OTA Translator
 * Device  : D2110-VE (serial ACCC8EF33E99)
 * Generated: 2026-07-13
 *
 * Port
 * - Detection Alert OTA uses dedicated port 133 for both requests and responses.
 *
 * Commands
 * - 0x01: GET_CONFIG request
 * - 0x81: GET_CONFIG response
 * - 0x02: SET_CONFIG request
 * - 0x82: SET_CONFIG ACK/NACK
 * - 0x03: GET_CAPS request
 * - 0x83: GET_CAPS response
 *
 * Config body format (48 bytes) for 0x81 and 0x02 body:
 * byte[0]   protocolVersion (uint8)
 * byte[1]   flags bit0=enabled bit1=labelVehicle bit2=aoiEnabled
 * byte[2]   heartbeatMinutes (uint8, 5..60)
 * byte[3:4] activeIntervalSeconds (uint16 BE, 60..300)
 * byte[5]   transitionSeconds (uint8, 2..20)
 * byte[6]   pointCount (uint8, 0 or 3..10)
 * byte[7:46] fixed point slots (10 points, each x,y uint16 BE)
 * byte[47]  crc8 over bytes[0..46]
 *
 * Public API
 * - Encode(config, command) : JSON -> HEX payload
 * - Decode(input)           : HEX -> JSON
 */

var AI_B100_ALERT_OTA_PORT = 133;
var AI_B100_ALERT_OTA_MAX_POINTS = 10;

function aiByte(value) { return value & 0xFF; }
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
  if (clean.length % 2 !== 0 || !/^[0-9A-F]+$/.test(clean)) throw new Error('HEX must be even-length hexadecimal string');
  var bytes = [];
  for (var i = 0; i < clean.length; i += 2) bytes.push(parseInt(clean.substr(i, 2), 16));
  return bytes;
}

function aiBytesToHex(bytes) {
  return (bytes || []).map(function(value) { return ('0' + aiByte(value).toString(16)).slice(-2); }).join('').toUpperCase();
}

function aiWriteU16BE(out, value) {
  var v = aiClamp(value, 0, 65535);
  out.push((v >> 8) & 0xFF);
  out.push(v & 0xFF);
}

function aiReadU16BE(bytes, index) {
  return (aiByte(bytes[index]) << 8) | aiByte(bytes[index + 1]);
}

function aiCrc8(bytes) {
  var crc = 0;
  for (var i = 0; i < bytes.length; i++) {
    crc ^= aiByte(bytes[i]);
    for (var bit = 0; bit < 8; bit++) {
      if (crc & 0x80) crc = ((crc << 1) ^ 0x07) & 0xFF;
      else crc = (crc << 1) & 0xFF;
    }
  }
  return crc & 0xFF;
}

function aiBuildConfigBody(config) {
  var cfg = config || {};
  var aoi = cfg.area || cfg.aoi || {};
  var points = (aoi.points || []).slice(0, AI_B100_ALERT_OTA_MAX_POINTS).map(function(point) {
    return { x: aiClamp(point.x, 0, 1000), y: aiClamp(point.y, 0, 1000) };
  });
  var aoiEnabled = !!aoi.enabled;
  if (aoiEnabled && points.length < 3) throw new Error('AOI enabled requires 3 to 10 points');

  var body = [];
  body.push(aiClamp(cfg.protocolVersion || 1, 1, 255));
  body.push((cfg.enabled ? 1 : 0) | (String(cfg.label || 'human').toLowerCase() === 'vehicle' ? 2 : 0) | (aoiEnabled ? 4 : 0));
  body.push(aiClamp(cfg.heartbeatMin || cfg.heartbeatMinutes, 5, 60));
  aiWriteU16BE(body, aiClamp(cfg.activeIntervalSec || cfg.activeIntervalSeconds, 60, 300));
  body.push(aiClamp(cfg.transitionSec || cfg.transitionSeconds, 2, 20));
  body.push(aoiEnabled ? points.length : 0);

  for (var i = 0; i < AI_B100_ALERT_OTA_MAX_POINTS; i++) {
    var point = i < points.length ? points[i] : { x: 0, y: 0 };
    aiWriteU16BE(body, point.x);
    aiWriteU16BE(body, point.y);
  }
  body.push(aiCrc8(body));
  if (body.length !== 48) throw new Error('Config body must be 48 bytes');
  return body;
}

function aiParseConfigBody(body) {
  if (!body || body.length !== 48) throw new Error('Config body must be 48 bytes');
  if (aiCrc8(body.slice(0, 47)) !== aiByte(body[47])) throw new Error('Config CRC mismatch');

  var flags = aiByte(body[1]);
  var pointCount = aiByte(body[6]);
  if (pointCount > AI_B100_ALERT_OTA_MAX_POINTS) throw new Error('Invalid pointCount');
  if ((flags & 0x04) && pointCount > 0 && pointCount < 3) throw new Error('AOI enabled with fewer than 3 points');

  var points = [];
  for (var i = 0; i < AI_B100_ALERT_OTA_MAX_POINTS; i++) {
    var base = 7 + (i * 4);
    var x = aiReadU16BE(body, base);
    var y = aiReadU16BE(body, base + 2);
    if (i < pointCount) points.push({ x: x, y: y });
  }

  return {
    version: aiByte(body[0]),
    enabled: (flags & 0x01) !== 0,
    label: (flags & 0x02) !== 0 ? 'vehicle' : 'human',
    heartbeatMin: aiByte(body[2]),
    activeIntervalSec: aiReadU16BE(body, 3),
    transitionSec: aiByte(body[5]),
    area: { enabled: (flags & 0x04) !== 0, points: points },
    crc8: aiByte(body[47])
  };
}

function Encode(config, command) {
  var cmd = aiByte(command);
  if (cmd === 0x01 || cmd === 0x03) return aiBytesToHex([cmd]);
  if (cmd !== 0x02) throw new Error('Unsupported command for JSON encode');
  return aiBytesToHex([0x02].concat(aiBuildConfigBody(config || {})));
}

function Decode(input) {
  var bytes = aiHexToBytes(input);
  if (!bytes.length) throw new Error('Empty payload');
  var command = aiByte(bytes[0]);
  var names = { 0x01: 'GET_CONFIG', 0x02: 'SET_CONFIG', 0x03: 'GET_CAPS', 0x81: 'GET_CONFIG_RESP', 0x82: 'SET_CONFIG_ACK', 0x83: 'GET_CAPS_RESP' };

  if (command === 0x01) return { port: AI_B100_ALERT_OTA_PORT, cmd: command, cmdName: names[command], type: 'request' };
  if (command === 0x03) return { port: AI_B100_ALERT_OTA_PORT, cmd: command, cmdName: names[command], type: 'request' };

  if (command === 0x02 || command === 0x81) {
    if (bytes.length !== 49) throw new Error('Config request/response must be 49 bytes');
    return {
      port: AI_B100_ALERT_OTA_PORT,
      cmd: command,
      cmdName: names[command],
      type: command === 0x02 ? 'request' : 'response',
      settings: aiParseConfigBody(bytes.slice(1))
    };
  }

  if (command === 0x82) {
    if (bytes.length !== 5) throw new Error('ACK/NACK must be 5 bytes');
    if (aiCrc8(bytes.slice(0, 4)) !== aiByte(bytes[4])) throw new Error('ACK/NACK CRC mismatch');
    return {
      port: AI_B100_ALERT_OTA_PORT,
      cmd: command,
      cmdName: names[command],
      type: 'ack',
      version: aiByte(bytes[1]),
      ackFor: aiByte(bytes[2]),
      status: aiByte(bytes[3]),
      success: aiByte(bytes[3]) === 0
    };
  }

  if (command === 0x83) {
    if (bytes.length < 13) throw new Error('Capabilities response must be 13 bytes');
    var payload = bytes.slice(1);
    if (aiCrc8(payload.slice(0, 11)) !== aiByte(payload[11])) throw new Error('Capabilities CRC mismatch');
    var coordEncoding = aiByte(payload[2]);
    var minHeartbeat = aiByte(payload[3]);
    var maxHeartbeat = aiByte(payload[4]);
    var minActive = aiReadU16BE(payload, 5);
    var maxActive = aiReadU16BE(payload, 7);
    var minTransition = aiByte(payload[9]);
    var maxTransition = aiByte(payload[10]);
    return {
      port: AI_B100_ALERT_OTA_PORT,
      cmd: command,
      cmdName: names[command],
      type: 'response',
      version: aiByte(payload[0]),
      maxPoints: aiByte(payload[1]),
      coordEncoding: coordEncoding,
      coordEncodingName: coordEncoding === 1 ? 'uint16_0_1000' : 'unknown',
      minHeartbeatMin: minHeartbeat,
      maxHeartbeatMin: maxHeartbeat,
      minActiveSec: minActive,
      maxActiveSec: maxActive,
      minTransitionSec: minTransition,
      maxTransitionSec: maxTransition,
      constraints: {
        heartbeatMin: [minHeartbeat, maxHeartbeat],
        activeIntervalSec: [minActive, maxActive],
        transitionSec: [minTransition, maxTransition],
        maxPoints: aiByte(payload[1])
      }
    };
  }

  throw new Error('Unsupported command byte: 0x' + command.toString(16).toUpperCase());
}

var aiB100AlertOtaJsonToHex = Encode;
var aiB100AlertOtaBufferToJson = Decode;
