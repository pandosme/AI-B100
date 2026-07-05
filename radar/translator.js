/**
 * Radar LoRaWAN Decoder
 *
 * Uplink payloads
 * - Port 1, Counting, 2 bytes:
 *   byte[0] = selected-label entering count, uint8, 0-255
 *   byte[1] = selected-label exiting count, uint8, 0-255
 * - Port 2, Occupancy Interval Maximum, 1 byte:
 *   byte[0] = selected-label maximum occupancy during interval, uint8, 0-255
 * - Port 3, Detection Alert, 1 byte:
 *   byte[0] = selected-label detection count, uint8, 0-255. 0 means inactive/no detections.
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
 */

function byteValue(value) {
  return value & 0xff;
}

function asciiFromBytes(bytes) {
  return bytes.map(function (value) { return String.fromCharCode(byteValue(value)); }).join('');
}

function numberFromText(value) {
  var parsed = Number(String(value || '').replace(/[^0-9.-]/g, ''));
  return Number.isFinite(parsed) ? parsed : null;
}

function decodeInformationRequest(bytes, fPort) {
  if (bytes.length !== 1) throw new Error('Information Request payload must be 1 byte');

  var code = byteValue(bytes[0]);
  var requests = {
    1: { requestName: 'camera_info', requestLabel: 'Camera Info', responsePort: 121 },
    2: { requestName: 'bridge_info', requestLabel: 'Bridge Info', responsePort: 122 },
    3: { requestName: 'signal_quality', requestLabel: 'Signal Quality', responsePort: null }
  };
  var request = requests[code] || { requestName: 'unknown', requestLabel: 'Unknown', responsePort: null };

  return {
    port: fPort,
    messageType: 'information_request',
    requestCode: code,
    requestName: request.requestName,
    requestLabel: request.requestLabel,
    responsePort: request.responsePort
  };
}

function decodeCameraInfo(bytes, fPort) {
  var text = asciiFromBytes(bytes);
  var fields = text.split(',');

  return {
    port: fPort,
    messageType: 'camera_info_response',
    raw: text,
    cameraInfo: {
      model: fields[0] || '',
      serial: fields[1] || '',
      firmwareVersion: fields[2] || '',
      uptimeHours: numberFromText(fields[3]),
      cpuPercent: numberFromText(fields[4]),
      appVersion: fields[5] || ''
    }
  };
}

function decodeBridgeInfo(bytes, fPort) {
  var text = asciiFromBytes(bytes);
  var fields = text.split(',');
  var hardwareParts = String(fields[0] || '').split('/');

  return {
    port: fPort,
    messageType: 'bridge_info_response',
    raw: text,
    bridgeInfo: {
      hardware: hardwareParts[0] || '',
      hardwareVersion: hardwareParts[1] || '',
      firmwareVersion: fields[1] || '',
      powerSource: fields[2] || '',
      temperatureC: numberFromText(fields[3]),
      restartCounter: numberFromText(fields[4]),
      devAddr: fields[5] || ''
    }
  };
}

function decodeRadarOccupancy(bytes, fPort) {
  if (!bytes) throw new Error('Missing bytes');

  if (fPort === 1) {
    if (bytes.length !== 2) throw new Error('Counting payload must be 2 bytes');
    return {
      port: fPort,
      messageType: 'counting',
      useCase: 'counting',
      useCaseLabel: 'Counting',
      entering: byteValue(bytes[0]),
      exiting: byteValue(bytes[1])
    };
  }

  if (fPort === 2) {
    if (bytes.length !== 1) throw new Error('Occupancy payload must be 1 byte');
    return {
      port: fPort,
      messageType: 'occupancy_interval_maximum',
      useCase: 'occupancy',
      useCaseLabel: 'Occupancy Interval Maximum',
      count: byteValue(bytes[0])
    };
  }

  if (fPort === 3) {
    if (bytes.length !== 1) throw new Error('Detection Alert payload must be 1 byte');
    var count = byteValue(bytes[0]);
    return {
      port: fPort,
      messageType: 'detection_alert',
      useCase: 'alert',
      useCaseLabel: 'Detection Alert',
      active: count > 0,
      count: count
    };
  }

  if (fPort === 120) return decodeInformationRequest(bytes, fPort);
  if (fPort === 121) return decodeCameraInfo(bytes, fPort);
  if (fPort === 122) return decodeBridgeInfo(bytes, fPort);

  throw new Error('Unsupported Radar LoRaWAN port: ' + fPort);
}

function decodeUplink(input) {
  try {
    return { data: decodeRadarOccupancy(input.bytes, input.fPort) };
  } catch (error) {
    return { errors: [error.message] };
  }
}

function decodeDownlink(input) {
  return decodeUplink(input);
}
