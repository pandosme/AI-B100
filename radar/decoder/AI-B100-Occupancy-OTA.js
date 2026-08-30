/**
 * AI-B100 Occupancy OTA Translator
 * Generated: 2026-07-13
 *
 * Port
 * - Occupancy OTA uses dedicated port 132 for both requests and responses.
 *
 * Public API
 * - Encode(config, command) : JSON -> HEX payload
 * - Decode(input)           : HEX -> JSON
 */

var AI_B100_OCCUPANCY_OTA_PORT = 132;

var AI_B100_OCCUPANCY_OTA_MAX_POINTS = 10;

function occupancyOtaByte(value) { return value & 0xff; }

function occupancyClamp(value, minValue, maxValue) {
	const n = Number(value);
	if (!Number.isFinite(n)) return minValue;
	if (n < minValue) return minValue;
	if (n > maxValue) return maxValue;
	return Math.round(n);
}

function occupancyOtaBytesToHex(bytes) {
	return (bytes || []).map(function(value) { return ('0' + occupancyOtaByte(value).toString(16)).slice(-2); }).join('').toUpperCase();
}

function occupancyOtaHexToBytes(hex) {
	const clean = String(hex || '').replace(/\s+/g, '').toUpperCase();
	if (!clean.length) return [];
	if (clean.length % 2 !== 0 || !/^[0-9A-F]+$/.test(clean)) throw new Error('HEX must be even-length hexadecimal string');
	const bytes = [];
	for (let i = 0; i < clean.length; i += 2) bytes.push(parseInt(clean.substr(i, 2), 16));
	return bytes;
}

function occupancyOtaWriteU16BE(out, value) {
	const v = occupancyClamp(value, 0, 65535);
	out.push((v >> 8) & 0xff);
	out.push(v & 0xff);
}

function occupancyOtaReadU16BE(bytes, index) {
	return (occupancyOtaByte(bytes[index]) << 8) | occupancyOtaByte(bytes[index + 1]);
}

function occupancyOtaCrc8(bytes) {
	let crc = 0;
	for (let i = 0; i < bytes.length; i++) {
		crc ^= occupancyOtaByte(bytes[i]);
		for (let bit = 0; bit < 8; bit++) {
			if (crc & 0x80) crc = ((crc << 1) ^ 0x07) & 0xff;
			else crc = (crc << 1) & 0xff;
		}
	}
	return crc & 0xff;
}

function occupancyTypeToCode(value) {
	if (typeof value !== 'string') throw new Error('config.type must be "Interval Maximum" or "Area Balance"');
	const t = value.trim().toLowerCase();
	if (t === 'area balance') return 2;
	if (t === 'interval maximum') return 1;
	throw new Error('config.type must be "Interval Maximum" or "Area Balance"');
}

function occupancyCodeToTypeName(code) {
	if (code === 2) return 'Area Balance';
	if (code === 1) return 'Interval Maximum';
	return 'Unknown';
}

function occupancyBuildConfigBody(config) {
	const cfg = config || {};
	const aoi = cfg.aoi || {};
	const points = (aoi.points || []).slice(0, 10).map(function(point) {
		return { x: occupancyClamp(point.x, 0, 1000), y: occupancyClamp(point.y, 0, 1000) };
	});
	const aoiEnabled = !!aoi.enabled;
	if (aoiEnabled && points.length < 3) throw new Error('AOI enabled requires 3 to 10 points');

	const body = [];
	body.push(occupancyClamp(cfg.protocolVersion || 1, 1, 255));
	body.push((cfg.enabled ? 1 : 0) | (String(cfg.label || 'human').toLowerCase() === 'vehicle' ? 2 : 0) | (aoiEnabled ? 4 : 0));
	body.push(occupancyClamp(cfg.intervalMin || cfg.intervalMinutes, 1, 60));
	body.push(aoiEnabled ? points.length : 0);

	for (let i = 0; i < 10; i++) {
		const point = i < points.length ? points[i] : { x: 0, y: 0 };
		occupancyOtaWriteU16BE(body, point.x);
		occupancyOtaWriteU16BE(body, point.y);
	}
	body.push(occupancyTypeToCode(cfg.type));
	body.push(0);
	body.push(0);
	body.push(occupancyOtaCrc8(body));

	if (body.length !== 48) throw new Error('Config body must be 48 bytes');
	return body;
}

function occupancyParseConfigBody(body) {
	if (!body || body.length !== 48) throw new Error('Config body must be 48 bytes');
	if (occupancyOtaCrc8(body.slice(0, 47)) !== occupancyOtaByte(body[47])) throw new Error('Config CRC mismatch');

	const flags = occupancyOtaByte(body[1]);
	const pointCount = occupancyOtaByte(body[3]);
	if (pointCount > 10) throw new Error('Invalid pointCount');
	if ((flags & 0x04) && pointCount > 0 && pointCount < 3) throw new Error('AOI enabled with fewer than 3 points');

	const points = [];
	for (let i = 0; i < 10; i++) {
		const base = 4 + (i * 4);
		const x = occupancyOtaReadU16BE(body, base);
		const y = occupancyOtaReadU16BE(body, base + 2);
		if (i < pointCount) points.push({ x, y });
	}

	return {
		protocolVersion: occupancyOtaByte(body[0]),
		enabled: (flags & 0x01) !== 0,
		label: (flags & 0x02) !== 0 ? 'vehicle' : 'human',
		intervalMinutes: occupancyOtaByte(body[2]),
		intervalMin: occupancyOtaByte(body[2]),
		type: occupancyCodeToTypeName(occupancyOtaByte(body[44])),
		aoi: { enabled: (flags & 0x04) !== 0, pointCount, points },
		crc8: occupancyOtaByte(body[47])
	};
}

function Encode(config, command) {
	const cmd = occupancyOtaByte(command);
	if (cmd === 0x01 || cmd === 0x03) return occupancyOtaBytesToHex([cmd]);
	if (cmd !== 0x02) throw new Error('Unsupported command for JSON encode');
	return occupancyOtaBytesToHex([0x02].concat(occupancyBuildConfigBody(config || {})));
}

function Decode(input) {
	const bytes = occupancyOtaHexToBytes(input);
	if (!bytes.length) throw new Error('Empty payload');

	const cmd = occupancyOtaByte(bytes[0]);
	const names = {
		0x01: 'GET_CONFIG',
		0x02: 'SET_CONFIG',
		0x03: 'GET_CAPS',
		0x81: 'GET_CONFIG_RESP',
		0x82: 'SET_CONFIG_ACK',
		0x83: 'GET_CAPS_RESP'
	};

	if (cmd === 0x01 || cmd === 0x03) return {
		port: 132,
		command: cmd,
		type: cmd === 0x01 ? 'get_config_request' : 'get_caps_request'
	};

	if (cmd === 0x02 || cmd === 0x81) {
		if (bytes.length !== 49) throw new Error('Config request/response must be 49 bytes');
		return {
			port: 132,
			command: cmd,
			type: cmd === 0x02 ? 'set_config_request' : 'get_config_response',
			config: occupancyParseConfigBody(bytes.slice(1))
		};
	}

	if (cmd === 0x82) {
		if (bytes.length !== 5) throw new Error('ACK/NACK must be 5 bytes');
		if (occupancyOtaCrc8(bytes.slice(0, 4)) !== occupancyOtaByte(bytes[4])) throw new Error('ACK/NACK CRC mismatch');
		return {
			port: 132,
			command: cmd,
			type: 'set_config_ack',
			version: occupancyOtaByte(bytes[1]),
			ackFor: occupancyOtaByte(bytes[2]),
			status: occupancyOtaByte(bytes[3]),
			success: occupancyOtaByte(bytes[3]) === 0
		};
	}

	if (cmd === 0x83) {
		if (bytes.length !== 13) throw new Error('Capabilities response must be 13 bytes');
		const payload = bytes.slice(1);
		if (occupancyOtaCrc8(payload.slice(0, 11)) !== occupancyOtaByte(payload[11])) throw new Error('Capabilities CRC mismatch');
		return {
			port: 132,
			command: cmd,
			type: 'get_caps_response',
			version: occupancyOtaByte(payload[0]),
			maxPolygonPoints: occupancyOtaByte(payload[1]),
			coordEncoding: occupancyOtaByte(payload[2]),
			coordEncodingName: occupancyOtaByte(payload[2]) === 1 ? 'uint16_0_1000' : 'unknown',
			minIntervalMin: occupancyOtaByte(payload[3]),
			maxIntervalMin: occupancyOtaByte(payload[4]),
			minAreaPoints: occupancyOtaReadU16BE(payload, 5),
			maxAreaPoints: occupancyOtaReadU16BE(payload, 7),
			minType: occupancyOtaByte(payload[9]),
			maxType: occupancyOtaByte(payload[10]),
			minTypeName: occupancyCodeToTypeName(occupancyOtaByte(payload[9])),
			maxTypeName: occupancyCodeToTypeName(occupancyOtaByte(payload[10])),
			constraints: {
				intervalMin: [occupancyOtaByte(payload[3]), occupancyOtaByte(payload[4])],
				areaPoints: [occupancyOtaReadU16BE(payload, 5), occupancyOtaReadU16BE(payload, 7)],
				occupancyType: [occupancyOtaByte(payload[9]), occupancyOtaByte(payload[10])],
				occupancyTypeName: [occupancyCodeToTypeName(occupancyOtaByte(payload[9])), occupancyCodeToTypeName(occupancyOtaByte(payload[10]))],
				maxPolygonPoints: occupancyOtaByte(payload[1])
			}
		};
	}

	throw new Error('Unsupported command byte: 0x' + cmd.toString(16).toUpperCase());
}

var aiB100OccupancyOtaJsonToHex = Encode;

var aiB100OccupancyOtaBufferToJson = Decode;
