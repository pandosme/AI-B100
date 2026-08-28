/** AI-B100 Radar OTA Encoder
 *
 * Usage:
 *   encodeRadarOta({ port: 111, command: 'set', transactionId: 1,
 *     config: { detectionSensitivity: 'medium' } });
 * Returns: { port: 111, payload: '...' }
 */
var RADAR_OTA_COMMANDS = { get: 0x01, set: 0x02, caps: 0x03, list: 0x04 };
var RADAR_OTA_SERVICES = { counting: 1, occupancy: 2, presence: 3, presenceAlert: 3 };
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

function radarOtaByte(value) {
	return Number(value) & 0xFF;
}

function radarOtaCrc8(values) {
	var crc = 0;
	values.forEach(function(value) {
		crc ^= radarOtaByte(value);
		for (var bit = 0; bit < 8; bit++)
			crc = (crc & 0x80) ? (((crc << 1) ^ 0x07) & 0xFF) : ((crc << 1) & 0xFF);
	});
	return crc;
}

function radarOtaHex(values) {
	return values.map(function(value) {
		return ('0' + radarOtaByte(value).toString(16)).slice(-2);
	}).join('').toUpperCase();
}

function radarOtaCommand(value) {
	if (typeof value === 'number') return radarOtaByte(value);
	var command = RADAR_OTA_COMMANDS[String(value || '').replace(/-/g, '_')];
	if (command == null) throw new Error('Unsupported command: ' + value);
	return command;
}

function radarOtaService(value) {
	if (typeof value === 'number') return Number(value);
	var service = RADAR_OTA_SERVICES[value];
	if (!service) throw new Error('Unsupported service: ' + value);
	return service;
}

function radarOtaInteger(value, minimum, maximum, name) {
	var number = Number(value);
	if (!Number.isInteger(number) || number < minimum || number > maximum)
		throw new Error(name + ' must be an integer from ' + minimum + ' through ' + maximum);
	return number;
}

function radarOtaFrame(command, transactionId, body) {
	var values = [radarOtaCommand(command), 1, radarOtaByte(transactionId || 0)].concat(body || []);
	if (values.length + 1 > 51) throw new Error('OTA frame exceeds 51 bytes');
	values.push(radarOtaCrc8(values));
	return radarOtaHex(values);
}

function radarOtaWriteU16BE(output, value) {
	output.push((value >> 8) & 0xFF, value & 0xFF);
}

function radarOtaWriteU16LE(output, value) {
	output.push(value & 0xFF, (value >> 8) & 0xFF);
}

function radarOtaMinutes(value, name) {
	if (typeof value !== 'string' || !/^\d{2}:\d{2}$/.test(value))
		throw new Error(name + ' must use HH:MM');
	var parts = value.split(':').map(Number);
	if (parts[0] > 23 || parts[1] > 59) throw new Error(name + ' must use a valid 24-hour time');
	return parts[0] * 60 + parts[1];
}

function radarOtaWritePackedPoint(output, point) {
	var x = radarOtaInteger(point.x, 0, 1000, 'point.x');
	var y = radarOtaInteger(point.y, 0, 1000, 'point.y');
	var packed = x + y * 0x400;
	output.push(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0x0F);
}

function radarOtaCountingBody(message) {
	var config = message.config || {};
	var name = String(config.name || '');
	if (!/^[\x20-\x7E]{1,31}$/.test(name)) throw new Error('name must contain 1 through 31 printable ASCII characters');
	var id = radarOtaInteger(config.id == null ? 0 : config.id, 0, 65535, 'id');
	var sceneIndex = radarOtaInteger(config.sceneIndex == null ? 0 : config.sceneIndex, 0, 10, 'sceneIndex');
	var sceneFingerprint = radarOtaInteger(config.sceneFingerprint || 0, 0, 65535, 'sceneFingerprint');
	var mapFingerprint = radarOtaInteger(config.mapFingerprint == null ? 0xFFFF : config.mapFingerprint, 0, 65535, 'mapFingerprint');
	var deleting = message.action === 'delete' || config.delete === true;
	var flags = deleting ? 0x80 : (config.enabled ? 1 : 0) |
		(config.direction === 'rightToLeft' ? 2 : 0) |
		(config.classes && config.classes.human ? 4 : 0) |
		(config.classes && config.classes.vehicle ? 8 : 0);
	var body = [1, sceneIndex];
	radarOtaWriteU16LE(body, id); radarOtaWriteU16LE(body, sceneFingerprint); radarOtaWriteU16LE(body, mapFingerprint);
	body.push(flags);
	var line = config.line || [{ x: 0, y: 0 }, { x: 0, y: 0 }];
	if (!deleting && (line.length !== 2 || (line[0].x === line[1].x && line[0].y === line[1].y)))
		throw new Error('line must contain two distinct points');
	radarOtaWritePackedPoint(body, line[0]); radarOtaWritePackedPoint(body, line[1]);
	body.push(name.length);
	for (var index = 0; index < name.length; index++) body.push(name.charCodeAt(index));
	return body;
}

function radarOtaServiceConfig(message, presence) {
	var config = message.config || {};
	var area = config.aoi || {};
	var points = area.points || config.points || [];
	if (points.length > 10 || (area.enabled && points.length < 3))
		throw new Error('Enabled AOI requires 3 through 10 points');
	var flags = (config.enabled ? 1 : 0) |
		(String(config.label || 'human').toLowerCase() === 'vehicle' ? 2 : 0) |
		(area.enabled ? 4 : 0);
	var body = [2, flags];
	if (presence) {
		body.push(radarOtaInteger(config.heartbeatMinutes, 5, 60, 'heartbeatMinutes'));
		radarOtaWriteU16LE(body, radarOtaInteger(config.activeIntervalSeconds, 60, 300, 'activeIntervalSeconds'));
		body.push(radarOtaInteger(config.transitionSeconds, 2, 20, 'transitionSeconds'), points.length);
		var schedule = config.schedule || {};
		body.push(schedule.enabled ? 1 : 0);
		radarOtaWriteU16LE(body, radarOtaMinutes(schedule.start || '18:00', 'schedule.start'));
		radarOtaWriteU16LE(body, radarOtaMinutes(schedule.end || '06:00', 'schedule.end'));
	} else {
		body.push(radarOtaInteger(config.intervalMinutes, 1, 60, 'intervalMinutes'), 1, points.length);
	}
	points.forEach(function(point) { radarOtaWritePackedPoint(body, point); });
	return body;
}

function radarOtaLegacyConfig(message, presence) {
	var config = message.config || {};
	var area = config.aoi || {};
	var points = (area.points || config.points || []).slice(0, 10);
	var areaEnabled = Boolean(area.enabled);
	if (areaEnabled && (points.length < 3 || points.length > 10))
		throw new Error('Enabled AOI requires 3 through 10 points');
	var flags = (config.enabled ? 1 : 0) |
		(String(config.label || 'human').toLowerCase() === 'vehicle' ? 2 : 0) |
		(areaEnabled ? 4 : 0);
	var body = [1, flags];

	if (presence) {
		body.push(radarOtaInteger(config.heartbeatMinutes, 5, 60, 'heartbeatMinutes'));
		radarOtaWriteU16BE(body, radarOtaInteger(config.activeIntervalSeconds, 60, 300, 'activeIntervalSeconds'));
		body.push(radarOtaInteger(config.transitionSeconds, 2, 20, 'transitionSeconds'));
		body.push(areaEnabled ? points.length : 0);
	} else {
		body.push(radarOtaInteger(config.intervalMinutes, 1, 60, 'intervalMinutes'));
		body.push(areaEnabled ? points.length : 0);
	}

	for (var index = 0; index < 10; index++) {
		var point = index < points.length ? points[index] : { x: 0, y: 0 };
		radarOtaWriteU16BE(body, radarOtaInteger(point.x, 0, 1000, 'point.x'));
		radarOtaWriteU16BE(body, radarOtaInteger(point.y, 0, 1000, 'point.y'));
	}
	if (!presence) body.push(1, 0, 0); // maximum, reserved, reserved
	if (body.length !== 47) throw new Error('Internal legacy body length error');
	body.push(radarOtaCrc8(body));
	return radarOtaHex([0x02].concat(body));
}

function encodeRadarOta(message) {
	message = message || {};
	var port = Number(message.port);
	var config = message.config || {};
	var commandName = message.command;
	if (commandName == null) {
		if (port === 100) commandName = 'set';
		else if (port === 110) commandName = config.dataRate != null || config.adrEnabled != null || config.adr != null ? 'set' : 'get';
		else if (port === 120) commandName = 'get';
		else if (port === 132 || port === 133) commandName = Object.keys(config).length ? 'set' : 'get';
	}
	var command = radarOtaCommand(commandName);

	if (port === 100) {
		var actionBody = [];
		if (command === 0x02) {
			var actions = { restartBridge: 1, resetAllData: 2 };
			var actionValue = message.action != null ? message.action : config.action;
			var action = typeof actionValue === 'number' ? actionValue : actions[actionValue];
			if (!action) throw new Error('action must be restartBridge or resetAllData');
			actionBody = [action];
		} else if (command !== 0x03) {
			throw new Error('Port 100 supports set and caps');
		}
		return { port: port, payload: radarOtaFrame(command, message.transactionId, actionBody) };
	}
	if (port === 110) {
		var bridgeBody = [];
		if (command === 0x02) {
			var fieldMask = 0;
			if (config.dataRate != null) fieldMask |= 0x01;
			if (config.adrEnabled != null || config.adr != null) fieldMask |= 0x02;
			if (!fieldMask) throw new Error('Set dataRate and/or adrEnabled');
			var dataRate = config.dataRate == null ? 0 : radarOtaInteger(config.dataRate, 0, 5, 'dataRate');
			var adrEnabled = config.adrEnabled != null ? config.adrEnabled : config.adr;
			bridgeBody = [fieldMask, dataRate, adrEnabled ? 1 : 0];
		} else if (command !== 0x01 && command !== 0x03) {
			throw new Error('Port 110 supports get, set, and caps');
		}
		return { port: port, payload: radarOtaFrame(command, message.transactionId, bridgeBody) };
	}
	if (port === 120) {
		var informationBody = [];
		if (command === 0x01) {
			var informationTypes = { camera: 1, cameraInfo: 1, bridge: 2, bridgeInfo: 2 };
			var informationValue = message.infoType != null ? message.infoType : (message.query != null ? message.query : config.service);
			var informationType = typeof informationValue === 'number' ? informationValue : informationTypes[informationValue];
			if (!informationType) throw new Error('infoType must be camera or bridge');
			informationBody = [informationType, radarOtaInteger(message.page || config.page || 0, 0, 255, 'page')];
		} else if (command !== 0x03) {
			throw new Error('Port 120 supports get and caps');
		}
		return { port: port, payload: radarOtaFrame(command, message.transactionId, informationBody) };
	}
	if (port === 111) {
		var radarBody = [];
		if (command === 0x02) {
			var sensitivity = { low: 1, medium: 2, high: 3 }[config.detectionSensitivity];
			if (!sensitivity) throw new Error('detectionSensitivity must be low, medium, or high');
			radarBody = [1, 0, sensitivity];
		}
		return { port: port, payload: radarOtaFrame(command, message.transactionId, radarBody) };
	}
	if (port === 130) {
		var transmissionBody = [];
		var service = null;
		if (command !== 0x03) {
			service = radarOtaService(message.service != null ? message.service : config.service);
			transmissionBody.push(service);
		}
		if (command === 0x02) {
			transmissionBody.push(config.enabled ? 1 : 0);
			if (service !== 3)
				transmissionBody.push(radarOtaInteger(config.intervalMinutes, 1, 60, 'intervalMinutes'));
		}
		return { port: port, payload: radarOtaFrame(command, message.transactionId, transmissionBody) };
	}
	if (port === 131) {
		var countingBody = [];
		if (command === 0x04) countingBody = [radarOtaInteger(message.page || 0, 0, 255, 'page')];
		else if (command === 0x01) countingBody = [radarOtaInteger(message.sceneIndex, 1, 10, 'sceneIndex'), 0];
		else if (command === 0x02) countingBody = radarOtaCountingBody(message);
		else if (command !== 0x03) throw new Error('Port 131 supports get, set, caps, and list');
		return { port: port, payload: radarOtaFrame(command, message.transactionId, countingBody) };
	}
	if (port === 132 || port === 133) {
		var serviceBody = [];
		if (command === 0x02) serviceBody = radarOtaServiceConfig(message, port === 133);
		else if (command !== 0x01 && command !== 0x03)
			throw new Error('Port ' + port + ' supports get, set, and caps');
		return { port: port, payload: radarOtaFrame(command, message.transactionId, serviceBody) };
	}
	throw new Error('Unsupported Radar OTA port: ' + port);
}

if (typeof module !== 'undefined') module.exports = {
	RADAR_OTA_GROUPS: RADAR_OTA_GROUPS,
	encodeRadarOta: encodeRadarOta
};