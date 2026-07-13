/**
 * Radar LoRaWAN Decoder
 * Device  : D2110-VE  (serial ACCC8EF33E99)
 * Generated: 2026-07-13
 *
 * Payloads
 * - Port 2, Occupancy, 1 byte: maximum selected-label objects during the interval.
 * - Port 3, Detection Alert, 1 byte: maximum selected-label objects while alert is active.
 *   Value 0 on port 3 means inactive or heartbeat.
 * - Port 1, Counting, 2 bytes: entering and exiting selected-label counts.
 *
 * Information request downlinks and responses
 * - Port 120, Information Request, 1 byte:
 *   byte[0] = 0x01 request Camera Info, response on port 121
 *   byte[0] = 0x02 request Bridge Info, response on port 122
 *   byte[0] = 0x03 request Signal Quality, local status update only
 * - Port 121, Camera Info response, ASCII CSV:
 *   model, serial, firmwareVersion, uptimeHours with h suffix, cpuPercent with percent suffix, appVersion
 * - Port 122, Bridge Info response, ASCII CSV:
 *   hardware/hardwareVersion, firmwareVersion, powerSource, temperatureC with C suffix, restartCounter with R prefix, devAddr
 *
 * The selected label, Humans or Vehicles, is configured in the ACAP UI and is not encoded in uplinks.
 *
 */

function Byte(value) { return value & 0xff; }
function HexToBytes(hex) {
  var clean = String(hex || '').replace(/\s+/g, '').toUpperCase();
  if (!clean.length) throw new Error('Missing hex payload');
  if (clean.length % 2 !== 0 || !/^[0-9A-F]+$/.test(clean)) throw new Error('Hex payload must be an even-length hexadecimal string');
  var bytes = [];
  for (var i = 0; i < clean.length; i += 2) bytes.push(parseInt(clean.substr(i, 2), 16));
  return bytes;
}
function Bytes(buffer) {
  if (!buffer) throw new Error('Missing buffer');
  if (typeof buffer === 'string') return HexToBytes(buffer);
  if (Array.isArray(buffer)) return buffer.map(Byte);
  if (typeof Uint8Array !== 'undefined' && buffer instanceof Uint8Array) return Array.prototype.slice.call(buffer).map(Byte);
  if (buffer.bytes) return Bytes(buffer.bytes);
  throw new Error('Payload must be a hex string, array, Buffer, or Uint8Array');
}
function RequireLength(bytes, length, name) {
  if (bytes.length !== length) throw new Error(name + ' payload must be ' + length + ' byte' + (length === 1 ? '' : 's'));
}
function Text(bytes) { return bytes.map(function(value) { return String.fromCharCode(Byte(value)); }).join(''); }
function NumberFromText(value) { var parsed = Number(String(value || '').replace(/[^0-9.-]/g, '')); return Number.isFinite(parsed) ? parsed : null; }

function Decode(buffer, port) {
  var bytes = Bytes(buffer);
  var fPort = Number(port);

  if (fPort === 2) {
    RequireLength(bytes, 1, 'Occupancy');
    return { port: fPort, useCase: 'occupancy', maximumObjects: Byte(bytes[0]) };
  }

  if (fPort === 3) {
    RequireLength(bytes, 1, 'Detection Alert');
    var maximumObjects = Byte(bytes[0]);
    return { port: fPort, useCase: 'detectionAlert', active: maximumObjects > 0, maximumObjects: maximumObjects };
  }

  if (fPort === 1) {
    RequireLength(bytes, 2, 'Counting');
    return { port: fPort, useCase: 'counting', enteringObjects: Byte(bytes[0]), exitingObjects: Byte(bytes[1]) };
  }

  if (fPort === 120) {
    RequireLength(bytes, 1, 'Information Request');
    var requests = { 1: 'cameraInfo', 2: 'bridgeInfo', 3: 'signalQuality' };
    return { port: fPort, request: requests[Byte(bytes[0])] || 'unknown', requestCode: Byte(bytes[0]) };
  }

  if (fPort === 121) {
    var cameraText = Text(bytes);
    var camera = cameraText.split(',');
    return { port: fPort, response: 'cameraInfo', raw: cameraText, model: camera[0] || '', serial: camera[1] || '', firmwareVersion: camera[2] || '', uptimeHours: NumberFromText(camera[3]), cpuPercent: NumberFromText(camera[4]), appVersion: camera[5] || '' };
  }

  if (fPort === 122) {
    var bridgeText = Text(bytes);
    var bridge = bridgeText.split(',');
    var hardware = String(bridge[0] || '').split('/');
    return { port: fPort, response: 'bridgeInfo', raw: bridgeText, hardware: hardware[0] || '', hardwareVersion: hardware[1] || '', firmwareVersion: bridge[1] || '', powerSource: bridge[2] || '', temperatureC: NumberFromText(bridge[3]), restartCounter: NumberFromText(bridge[4]), devAddr: bridge[5] || '' };
  }

  throw new Error('Unsupported Radar LoRaWAN port: ' + fPort);
}

function decodeUplink(input) {
  try {
    return { data: Decode(input.bytes, input.fPort) };
  } catch (error) {
    return { errors: [error.message] };
  }
}

function decodeDownlink(input) {
  return decodeUplink(input);
}
