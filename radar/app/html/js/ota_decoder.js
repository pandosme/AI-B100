/** AI-B100 Radar OTA Decoder
 *
 * Usage: decodeRadarOta(111, '810101010002...')
 */
var RADAR_OTA_COMMAND_NAMES = {
	1: 'get', 2: 'set', 3: 'caps', 4: 'list', 129: 'get_response',
	130: 'set_ack', 131: 'caps_response', 132: 'list_response', 224: 'error'
};
var RADAR_OTA_STATUS_NAMES = {
	0: 'ok', 1: 'invalid_length', 2: 'invalid_value', 3: 'invalid_range',
	4: 'crc_mismatch', 5: 'unknown_command', 6: 'unknown_scene',
	7: 'scene_fingerprint_mismatch', 8: 'map_fingerprint_mismatch',
	9: 'partial_page_pending', 10: 'apply_failed', 11: 'unsupported'
};
var RADAR_OTA_GROUPS = {
	actions: 100,
	loraConfiguration: 110,
	radarConfiguration: 111,
	information: 120,
	transmission: 130,
	counting: 131,
	occupancy: 132,
	presenceAlert: 133
};

function radarOtaDecodeCrc8(values) {
	var crc = 0;
	values.forEach(function(value) {
		crc ^= value & 0xFF;
		for (var bit = 0; bit < 8; bit++)
			crc = (crc & 0x80) ? (((crc << 1) ^ 0x07) & 0xFF) : ((crc << 1) & 0xFF);
	});
	return crc;
}

function radarOtaDecodeBytes(payload) {
	if (Array.isArray(payload)) return payload.map(function(value) { return Number(value) & 0xFF; });
	var text = String(payload || '').replace(/\s+/g, '');
	if (!text.length || text.length % 2 || !/^[0-9a-f]+$/i.test(text))
		throw new Error('Payload must be even-length HEX');
	var values = [];
	for (var index = 0; index < text.length; index += 2)
		values.push(parseInt(text.slice(index, index + 2), 16));
	return values;
}

function radarOtaReadU16BE(values, offset) {
	return ((values[offset] & 0xFF) << 8) | (values[offset + 1] & 0xFF);
}

function radarOtaReadU16LE(values, offset) {
	return (values[offset] & 0xFF) | ((values[offset + 1] & 0xFF) << 8);
}

function radarOtaReadU32LE(values, offset) {
	return (values[offset] & 0xFF) + ((values[offset + 1] & 0xFF) << 8) +
		((values[offset + 2] & 0xFF) << 16) + ((values[offset + 3] & 0xFF) * 0x1000000);
}

function radarOtaDecodeFrame(values) {
	if (values.length < 4 || values.length > 51) throw new Error('Invalid framed OTA length');
	if (radarOtaDecodeCrc8(values.slice(0, -1)) !== values[values.length - 1])
		throw new Error('Frame CRC mismatch');
	if (values[1] !== 1) throw new Error('Unsupported OTA protocol version');
	return {
		commandCode: values[0],
		command: RADAR_OTA_COMMAND_NAMES[values[0]] || 'unknown',
		version: values[1],
		transactionId: values[2],
		body: values.slice(3, -1)
	};
}

function radarOtaDecodeStructuredInformation(decoded, body) {
	var infoNames = { 1: 'cameraInfo', 2: 'bridgeInfo' };
	decoded.infoTypeCode = body[0];
	decoded.infoType = infoNames[body[0]] || 'unknown';
	var camera = body[0] === 1;
	var fixedLength = camera ? 10 : 11;
	if (body[0] !== 1 && body[0] !== 2) throw new Error('Unknown structured information type');
	if (body.length < fixedLength || body[1] !== 0x81) throw new Error('Invalid structured information body');
	var fieldLengths = body.slice(2, camera ? 6 : 7);
	var expectedLength = fixedLength + fieldLengths.reduce(function(sum, length) { return sum + length; }, 0);
	if (body.length !== expectedLength) throw new Error('Invalid structured information length');
	var offset = fixedLength;
	function readString(length) {
		var value = String.fromCharCode.apply(null, body.slice(offset, offset + length));
		offset += length;
		return value;
	}
	if (camera) {
		decoded.model = readString(fieldLengths[0]);
		decoded.serial = readString(fieldLengths[1]);
		decoded.firmware = readString(fieldLengths[2]);
		decoded.appVersion = readString(fieldLengths[3]);
		decoded.uptimeHours = radarOtaReadU32LE(body, 6);
	} else {
		decoded.hardware = readString(fieldLengths[0]);
		decoded.hardwareVersion = readString(fieldLengths[1]);
		decoded.firmware = readString(fieldLengths[2]);
		decoded.powerSource = readString(fieldLengths[3]);
		decoded.devAddr = readString(fieldLengths[4]);
		var temperatureTenths = radarOtaReadU16LE(body, 7);
		if (temperatureTenths & 0x8000) temperatureTenths -= 0x10000;
		decoded.temperatureC = temperatureTenths / 10;
		decoded.restartCounter = radarOtaReadU16LE(body, 9);
	}
	return decoded;
}

function radarOtaDecodePoints(body, offset, count) {
	var points = [];
	for (var index = 0; index < count; index++) {
		var base = offset + index * 4;
		points.push({ x: radarOtaReadU16BE(body, base), y: radarOtaReadU16BE(body, base + 2) });
	}
	return points;
}

function radarOtaDecodePackedPoints(body, offset, count) {
	var points = [];
	for (var index = 0; index < count; index++) {
		var base = offset + index * 3;
		if (body[base + 2] & 0xF0) throw new Error('Invalid packed coordinate');
		var packed = body[base] + body[base + 1] * 0x100 + body[base + 2] * 0x10000;
		var x = packed & 0x3FF;
		var y = (packed >> 10) & 0x3FF;
		if (x > 1000 || y > 1000) throw new Error('Coordinate outside 0 through 1000');
		points.push({ x: x, y: y });
	}
	return points;
}

function radarOtaDecodeCountingConfig(decoded, body) {
	if (body.length < 16 || body[0] !== 1 || body.length !== 16 + body[15])
		throw new Error('Invalid Counting scene configuration');
	var flags = body[8];
	decoded.configVersion = body[0]; decoded.sceneIndex = body[1];
	decoded.sceneId = radarOtaReadU16LE(body, 2);
	decoded.sceneFingerprint = radarOtaReadU16LE(body, 4);
	decoded.mapFingerprint = radarOtaReadU16LE(body, 6);
	decoded.config = {
		id: decoded.sceneId,
		name: String.fromCharCode.apply(null, body.slice(16)),
		enabled: Boolean(flags & 1),
		direction: flags & 2 ? 'rightToLeft' : 'leftToRight',
		classes: { human: Boolean(flags & 4), vehicle: Boolean(flags & 8) },
		line: radarOtaDecodePackedPoints(body, 9, 2)
	};
	return decoded;
}

function radarOtaHHMM(minutes) {
	if (minutes < 0 || minutes > 1439) throw new Error('Invalid schedule minute value');
	return String(Math.floor(minutes / 60)).padStart(2, '0') + ':' + String(minutes % 60).padStart(2, '0');
}

function radarOtaDecodeServiceConfig(port, decoded, body) {
	var presence = port === 133;
	var fixedLength = presence ? 12 : 5;
	if (body.length < fixedLength || body[0] !== 2) throw new Error('Invalid service configuration version');
	var flags = body[1];
	var pointCount = body[presence ? 6 : 4];
	if (pointCount > 10 || body.length !== fixedLength + pointCount * 3)
		throw new Error('Invalid service configuration length');
	var config = {
		enabled: Boolean(flags & 1),
		label: flags & 2 ? 'vehicle' : 'human',
		aoi: { enabled: Boolean(flags & 4), points: radarOtaDecodePackedPoints(body, fixedLength, pointCount) }
	};
	if (presence) {
		config.heartbeatMinutes = body[2];
		config.activeIntervalSeconds = radarOtaReadU16LE(body, 3);
		config.transitionSeconds = body[5];
		config.schedule = {
			enabled: Boolean(body[7]),
			start: radarOtaHHMM(radarOtaReadU16LE(body, 8)),
			end: radarOtaHHMM(radarOtaReadU16LE(body, 10))
		};
	} else {
		config.intervalMinutes = body[2];
		config.valueType = body[3] === 1 ? 'maximum' : 'unknown';
	}
	decoded.config = config;
	return decoded;
}

function radarOtaDecodeLegacyConfig(port, values) {
	var command = values[0];
	if (command === 0x01 || command === 0x03)
		return { port: port, commandCode: command, command: RADAR_OTA_COMMAND_NAMES[command], direction: 'request' };
	if (command === 0x82) {
		if (values.length !== 5 || radarOtaDecodeCrc8(values.slice(0, 4)) !== values[4])
			throw new Error('Invalid legacy acknowledgement');
		return { port: port, commandCode: command, command: 'set_ack', version: values[1], echoedCommand: values[2], statusCode: values[3], status: RADAR_OTA_STATUS_NAMES[values[3]] || 'unknown' };
	}
	if (command === 0x83) {
		if (values.length !== 13 || radarOtaDecodeCrc8(values.slice(1, 12)) !== values[12])
			throw new Error('Invalid legacy capabilities response');
		if (port === 132) return { port: port, commandCode: command, command: 'caps_response', version: values[1], maxPoints: values[2], coordinateEncoding: 'uint16_0_1000', intervalMinutes: { min: values[4], max: values[5] }, areaPoints: { min: radarOtaReadU16BE(values, 6), max: radarOtaReadU16BE(values, 8) }, valueType: 'maximum' };
		return { port: port, commandCode: command, command: 'caps_response', version: values[1], maxPoints: values[2], coordinateEncoding: 'uint16_0_1000', heartbeatMinutes: { min: values[4], max: values[5] }, activeIntervalSeconds: { min: radarOtaReadU16BE(values, 6), max: radarOtaReadU16BE(values, 8) }, transitionSeconds: { min: values[10], max: values[11] } };
	}
	if (command !== 0x02 && command !== 0x81) throw new Error('Unsupported legacy command');
	if (values.length !== 49) throw new Error('Legacy configuration frame must be 49 bytes');
	var body = values.slice(1);
	if (radarOtaDecodeCrc8(body.slice(0, 47)) !== body[47]) throw new Error('Configuration CRC mismatch');
	var flags = body[1];
	var count = body[port === 132 ? 3 : 6];
	var config = {
		enabled: Boolean(flags & 1),
		label: (flags & 2) ? 'vehicle' : 'human',
		aoi: { enabled: Boolean(flags & 4), points: radarOtaDecodePoints(body, port === 132 ? 4 : 7, count) }
	};
	if (port === 132) {
		config.intervalMinutes = body[2];
		config.valueType = body[44] === 1 ? 'maximum' : 'unknown';
	} else {
		config.heartbeatMinutes = body[2];
		config.activeIntervalSeconds = radarOtaReadU16BE(body, 3);
		config.transitionSeconds = body[5];
	}
	return { port: port, commandCode: command, command: command === 2 ? 'set' : 'get_response', version: body[0], config: config };
}

function radarOtaDecodeAscii(port, payload) {
	var text = String(payload || '');
	if (/^[0-9a-f]+$/i.test(text) && text.length % 2 === 0) {
		var bytes = radarOtaDecodeBytes(text);
		text = bytes.map(function(value) { return String.fromCharCode(value); }).join('');
	}
	return { port: port, direction: 'response', text: text, fields: text.split(',') };
}

function decodeRadarOta(port, payload) {
	port = Number(port);
	if (port === 121 || port === 122) return radarOtaDecodeAscii(port, payload);
	var values = radarOtaDecodeBytes(payload);
	if ((port === 132 || port === 133) && (values.length < 4 || values[1] !== 1 ||
		radarOtaDecodeCrc8(values.slice(0, -1)) !== values[values.length - 1]))
		return radarOtaDecodeLegacyConfig(port, values);
	if ([100, 110, 111, 120, 130, 131, 132, 133].indexOf(port) < 0) throw new Error('Unsupported Radar OTA port: ' + port);
	var decoded = radarOtaDecodeFrame(values);
	decoded.port = port;
	var body = decoded.body;
	if (decoded.commandCode === 0xE0) {
		if (body.length !== 2) throw new Error('Invalid error response length');
		decoded.requestCommand = body[0];
		decoded.statusCode = body[1];
		decoded.status = RADAR_OTA_STATUS_NAMES[body[1]] || 'unknown';
	} else if (port === 100 && decoded.commandCode === 0x02) {
		if (body.length !== 1) throw new Error('Invalid action request length');
		decoded.actionCode = body[0];
		decoded.action = ({ 1: 'restartBridge', 2: 'resetAllData' })[body[0]] || 'unknown';
	} else if (port === 100 && decoded.commandCode === 0x82) {
		if (body.length !== 2) throw new Error('Invalid action acknowledgement length');
		decoded.actionCode = body[0];
		decoded.action = ({ 1: 'restartBridge', 2: 'resetAllData' })[body[0]] || 'unknown';
		decoded.statusCode = body[1];
		decoded.status = RADAR_OTA_STATUS_NAMES[body[1]] || 'unknown';
	} else if (port === 100 && decoded.commandCode === 0x83) {
		decoded.actions = body.map(function(action) { return ({ 1: 'restartBridge', 2: 'resetAllData' })[action] || action; });
	} else if (port === 110 && (decoded.commandCode === 0x02 || decoded.commandCode === 0x81)) {
		if (body.length !== 3) throw new Error('Invalid LoRa configuration length');
		decoded.config = { fieldMask: body[0], dataRate: body[1], adrEnabled: Boolean(body[2]) };
	} else if (port === 110 && decoded.commandCode === 0x82) {
		if (body.length !== 1) throw new Error('Invalid LoRa acknowledgement length');
		decoded.statusCode = body[0];
		decoded.status = RADAR_OTA_STATUS_NAMES[body[0]] || 'unknown';
	} else if (port === 110 && decoded.commandCode === 0x83) {
		if (body.length !== 3) throw new Error('Invalid LoRa capabilities length');
		decoded.capabilities = { dataRate: { min: body[0], max: body[1] }, adrSupported: Boolean(body[2]) };
	} else if (port === 120 && decoded.commandCode === 0x01) {
		if (body.length < 1 || body.length > 2) throw new Error('Invalid information request length');
		decoded.infoTypeCode = body[0];
		decoded.infoType = ({ 1: 'cameraInfo', 2: 'bridgeInfo' })[body[0]] || 'unknown';
		decoded.page = body[1] || 0;
	} else if (port === 120 && decoded.commandCode === 0x81) {
		if (body.length >= 2 && body[1] === 0x81) {
			radarOtaDecodeStructuredInformation(decoded, body);
		} else {
			if (body.length < 4 || body.length !== body[3] + 4) throw new Error('Invalid paged information length');
			decoded.infoTypeCode = body[0];
			decoded.infoType = ({ 1: 'cameraInfo', 2: 'bridgeInfo' })[body[0]] || 'unknown';
			decoded.page = body[1];
			decoded.pageCount = body[2];
			decoded.text = String.fromCharCode.apply(null, body.slice(4));
		}
	} else if (port === 120 && decoded.commandCode === 0x83) {
		if (body.length !== 3) throw new Error('Invalid information capabilities length');
		decoded.informationTypes = body.slice(0, 2).map(function(type) { return ({ 1: 'cameraInfo', 2: 'bridgeInfo' })[type] || type; });
		decoded.legacyTextBytesPerPage = body[2];
	} else if ((port === 132 || port === 133) && (decoded.commandCode === 0x02 || decoded.commandCode === 0x81)) {
		radarOtaDecodeServiceConfig(port, decoded, body);
	} else if ((port === 132 || port === 133) && decoded.commandCode === 0x82) {
		if (body.length !== 1) throw new Error('Invalid service acknowledgement length');
		decoded.statusCode = body[0];
		decoded.status = RADAR_OTA_STATUS_NAMES[body[0]] || 'unknown';
	} else if (port === 132 && decoded.commandCode === 0x83) {
		if (body.length !== 9) throw new Error('Invalid occupancy capabilities length');
		decoded.capabilities = { configVersion: body[0], maxPoints: body[1], coordinateEncoding: 'packed_10bit', areaPoints: { min: body[3], max: body[4] }, intervalMinutes: { min: body[5], max: body[6] }, valueType: 'maximum' };
	} else if (port === 133 && decoded.commandCode === 0x83) {
		if (body.length !== 14) throw new Error('Invalid Presence capabilities length');
		decoded.capabilities = { configVersion: body[0], maxPoints: body[1], coordinateEncoding: 'packed_10bit', areaPoints: { min: body[3], max: body[4] }, heartbeatMinutes: { min: body[5], max: body[6] }, activeIntervalSeconds: { min: radarOtaReadU16LE(body, 7), max: radarOtaReadU16LE(body, 9) }, transitionSeconds: { min: body[11], max: body[12] }, scheduleSupported: Boolean(body[13]) };
	} else if (port === 111 && (decoded.commandCode === 0x02 || decoded.commandCode === 0x81)) {
		decoded.config = { fieldMask: body[0] | (body[1] << 8), detectionSensitivity: ({ 1: 'low', 2: 'medium', 3: 'high' })[body[2]] || 'unknown' };
	} else if (port === 111 && decoded.commandCode === 0x83) {
		decoded.capabilities = { fieldMask: body[0] | (body[1] << 8), detectionSensitivity: { min: body[2], max: body[3] } };
	} else if (port === 111 && decoded.commandCode === 0x82) {
		decoded.statusCode = body[0];
		decoded.status = RADAR_OTA_STATUS_NAMES[body[0]] || 'unknown';
	} else if (port === 130 && (decoded.commandCode === 0x01 || decoded.commandCode === 0x02 || decoded.commandCode === 0x81)) {
		decoded.service = body[0];
		if (body.length > 1) decoded.enabled = Boolean(body[1]);
		if (body.length > 2) decoded.intervalMinutes = body[2];
	} else if (port === 130 && decoded.commandCode === 0x82) {
		decoded.service = body[0];
		decoded.statusCode = body[1];
		decoded.status = RADAR_OTA_STATUS_NAMES[body[1]] || 'unknown';
	} else if (port === 130 && decoded.commandCode === 0x83) {
		decoded.services = [];
		for (var index = 0; index + 3 < body.length; index += 4)
			decoded.services.push({ service: body[index], supportsInterval: Boolean(body[index + 1]), intervalMin: body[index + 2], intervalMax: body[index + 3] });
	} else if (port === 131 && decoded.commandCode === 0x01) {
		if (body.length !== 2 || body[1] !== 0) throw new Error('Invalid Counting get request');
		decoded.sceneIndex = body[0]; decoded.page = body[1];
	} else if (port === 131 && decoded.commandCode === 0x04) {
		if (body.length !== 1) throw new Error('Invalid Counting list request');
		decoded.page = body[0];
	} else if (port === 131 && (decoded.commandCode === 0x02 || decoded.commandCode === 0x81)) {
		radarOtaDecodeCountingConfig(decoded, body);
		if (decoded.commandCode === 0x02) decoded.delete = Boolean(body[8] & 0x80);
	} else if (port === 131 && decoded.commandCode === 0x82) {
		if (body.length !== 8) throw new Error('Invalid Counting acknowledgement');
		decoded.statusCode = body[0]; decoded.status = RADAR_OTA_STATUS_NAMES[body[0]] || 'unknown';
		decoded.sceneIndex = body[1]; decoded.sceneId = radarOtaReadU16LE(body, 2);
		decoded.sceneFingerprint = radarOtaReadU16LE(body, 4); decoded.mapFingerprint = radarOtaReadU16LE(body, 6);
	} else if (port === 131 && decoded.commandCode === 0x83) {
		if (body.length !== 5) throw new Error('Invalid Counting capabilities');
		decoded.capabilities = { configVersion: body[0], maxScenes: body[1], maxNameBytes: body[2], pointsPerScene: body[3], classMask: body[4] };
	} else if (port === 131 && decoded.commandCode === 0x84) {
		if (body.length < 6 || body.length !== 6 + body[5] * 6) throw new Error('Invalid Counting scene list');
		decoded.mapFingerprint = radarOtaReadU16LE(body, 0); decoded.totalScenes = body[2]; decoded.page = body[3]; decoded.pageCount = body[4]; decoded.scenes = [];
		for (var sceneOffset = 6; sceneOffset < body.length; sceneOffset += 6)
			decoded.scenes.push({ sceneIndex: body[sceneOffset], id: radarOtaReadU16LE(body, sceneOffset + 1), fingerprint: radarOtaReadU16LE(body, sceneOffset + 3), pointCount: body[sceneOffset + 5] });
	}
	return decoded;
}

if (typeof module !== 'undefined') module.exports = {
	RADAR_OTA_GROUPS: RADAR_OTA_GROUPS,
	decodeRadarOta: decodeRadarOta
};