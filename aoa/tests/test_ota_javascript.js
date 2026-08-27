'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const source = fs.readFileSync('app/ota_translator.c', 'utf8');

function extract(name) {
	const start = source.indexOf(`static const char* ${name} =`);
	assert(start >= 0, `Missing ${name}`);
	const end = source.indexOf(';\n\n', start);
	assert(end >= 0, `Missing end of ${name}`);
	const block = source.slice(start, end);
	const strings = block.match(/"(?:\\.|[^"\\])*"/g) || [];
	return strings.map((text) => JSON.parse(text)).join('');
}

const maps = {
	131: { mapFingerprint: 1001, scenes: [
		{ index: 1, id: 2, name: 'Entry', fingerprint: 2000, pointCount: 2 },
		{ index: 2, id: 7, name: 'Gate', fingerprint: 2001, pointCount: 10 }
	] },
	132: { mapFingerprint: 1002, scenes: [{ index: 1, id: 8, name: 'Bay', fingerprint: 2002, pointCount: 4 }] },
	133: { mapFingerprint: 1003, scenes: [{ index: 1, id: 9, name: 'Door', fingerprint: 2003, pointCount: 4 }] }
};

function load(templateName) {
	const sandbox = { console };
	vm.createContext(sandbox);
	vm.runInContext(`var OTA_SCENE_MAPS=${JSON.stringify(maps)};\n${extract('OTA_JS_COMMON')}\n${extract(templateName)}`, sandbox);
	return sandbox;
}

const encoder = load('OTA_ENCODER_JS');
const decoder = load('OTA_DECODER_JS');

function bodyOf(message) {
	return Array.from(Buffer.from(message, 'hex')).slice(3, -1);
}

function response(port, command, body, transactionId = 0) {
	return decoder.Decode(port, decoder.makeFrame(command, transactionId, body));
}

const transmission = encoder.Encode({
	type: 'Publish',
	config: { service: 'counting', active: true, intervall: 5 }
});
assert.strictEqual(transmission.port, 130);
assert.strictEqual(transmission.message, '02010001010555');
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(decoder.Decode(transmission).config)),
	{ useCase: 'counting', enabled: true, intervalMinutes: 5 }
);

const presence = encoder.Encode({
	type: 'Publish',
	config: { service: 'presence', active: false }
});
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(decoder.Decode(presence).config)),
	{ useCase: 'presence', enabled: false }
);

const points = Array.from({ length: 10 }, (_, index) => ({
	x: Math.round(index * 1000 / 9),
	y: Math.round(1000 - index * 1000 / 9)
}));
const counting = encoder.Encode({
	type: 'Counting',
	scene: 'Gate',
	config: {
		direction: 'rightToLeft',
		publishClasses: { human: true, car: true },
		points
	}
});
assert.strictEqual(counting.port, 131);
assert.strictEqual(counting.messages, undefined);
assert.strictEqual(counting.message.length / 2, 50);
const decodedCounting = decoder.Decode(counting);
assert.strictEqual(decodedCounting.scene.name, 'Gate');
assert.strictEqual(decodedCounting.config.direction, 'rightToLeft');
assert.strictEqual(decodedCounting.config.points.length, 10);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(decodedCounting.config.points)),
	points
);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(decodedCounting.coordinateSystem)),
	{ origin: 'topLeft', minimum: 0, maximum: 1000 }
);

const byIndex = encoder.Encode({
	port: 131,
	command: 'get',
	sceneIndex: 2
});
assert.strictEqual(decoder.Decode(byIndex).scene.name, 'Gate');

const getTransmission = decoder.Decode(encoder.Encode({
	port: 130,
	command: 'get',
	useCase: 'occupancy'
}));
assert.strictEqual(getTransmission.useCase, 'occupancy');

const listScenes = decoder.Decode(encoder.Encode({
	port: 132,
	command: 'list',
	page: 0
}));
assert.strictEqual(listScenes.command, 'list');
assert.strictEqual(listScenes.page, 0);

const countingListResponse = decoder.Decode(131, '8401007D8A01000101010200FA020264');
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(countingListResponse)),
	{
		port: 131,
		command: 'list_response',
		commandCode: 132,
		transactionId: 0,
		mapFingerprint: 35453,
		totalScenes: 1,
		page: 0,
		pageCount: 1,
		scenes: [{ index: 1, id: 2, fingerprint: 762, pointCount: 2, name: 'Entry' }]
	}
);

const entry = encoder.Encode({
	type: 'Counting',
	scene: 'Entry',
	config: {
		direction: 'leftToRight',
		publishClasses: { human: true },
		points: points.slice(0, 2)
	}
});
assert.throws(
	() => decoder.Decode({ port: 131, messages: [counting.message, entry.message] }),
	/Mismatched scene pages|Incomplete scene pages/
);

assert.throws(() => encoder.Encode({
	port: 131,
	command: 'set',
	scene: 'Entry',
	config: {
		direction: 'leftToRight',
		publishClasses: { human: true },
		points: [{ x: Number.NaN, y: 0 }, { x: 500, y: 1000 }]
	}
}), /integers from 0 through 1000/);

assert.throws(() => encoder.Encode({
	port: 131,
	command: 'set',
	scene: 'Entry',
	config: {
		direction: 'leftToRight',
		publishClasses: { human: true },
		points: [{ x: 0.5, y: 0 }, { x: 500, y: 1000 }]
	}
}), /integers from 0 through 1000/);

const legacyQ15Body = [
	1, 1, 2, 0, 0xD0, 0x07, 0xE9, 0x03,
	0, 1, 0, 2, 2, 0, 1, 0,
	0x01, 0x80, 0xFF, 0x7F,
	0, 0, 0, 0
];
const legacyQ15 = response(131, 0x81, legacyQ15Body);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(legacyQ15.config.points)),
	[{ x: 0, y: 1000 }, { x: 500, y: 500 }]
);

const action = encoder.Encode({ type: 'Action', config: { action: 'restartBridge' } });
assert.strictEqual(action.port, 100);
assert.deepStrictEqual(bodyOf(action.message), [1]);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(response(100, 0x82, [1, 0]))),
	{
		port: 100, command: 'set_ack', commandCode: 130, transactionId: 0,
		status: 0, statusName: 'ok', action: 'restartBridge'
	}
);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(response(100, 0x83, [1, 2]).actions)),
	['restartBridge', 'resetAllData']
);

const lora = encoder.Encode({ type: 'LoRaWAN', config: { dataRate: 3, adr: true } });
assert.strictEqual(lora.port, 110);
assert.deepStrictEqual(bodyOf(lora.message), [3, 3, 1]);
assert.strictEqual(decoder.Decode(lora).config.adrEnabled, true);
assert.deepStrictEqual(bodyOf(encoder.Encode({ type: 'LoRaWAN' }).message), []);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(response(110, 0x81, [3, 4, 0]).config)),
	{ fieldMask: 3, dataRate: 4, adrEnabled: false }
);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(response(110, 0x82, [0]))),
	{
		port: 110, command: 'set_ack', commandCode: 130, transactionId: 0,
		status: 0, statusName: 'ok'
	}
);
const loraCapabilities = response(110, 0x83, [0, 5, 1]);
assert.deepStrictEqual(JSON.parse(JSON.stringify(loraCapabilities.dataRate)), { minimum: 0, maximum: 5 });
assert.strictEqual(loraCapabilities.adrSupported, true);

const information = encoder.Encode({ type: 'Information', config: { service: 'camera', page: 0 } });
assert.strictEqual(information.port, 120);
assert.strictEqual(information.message, '010100010061');
assert.strictEqual(decoder.Decode(information).infoType, 'cameraInfo');
assert.strictEqual(
	encoder.Encode({ type: 'Information', config: { service: 'camera', page: 1 } }).message,
	'010100010166'
);
assert.deepStrictEqual(bodyOf(encoder.Encode({ type: 'Information', config: { service: 'bridge' } }).message), [2, 0]);
const informationCapabilities = response(120, 0x83, [1, 2, 43]);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(informationCapabilities.informationTypes)),
	['cameraInfo', 'bridgeInfo']
);
assert.strictEqual(informationCapabilities.legacyTextBytesPerPage, 43);

const cameraPage0 = '8101000100022B51333534382D4C56452C4238413434464631314133352C31322E31312E37372C393831682C3833252C322E28';
const cameraPage1 = '81010001010203302E309C';
const decodedCameraPage0 = decoder.Decode(120, cameraPage0);
assert.strictEqual(decodedCameraPage0.model, 'Q3548-LVE');
assert.strictEqual(decodedCameraPage0.serial, 'B8A44FF11A35');
assert.strictEqual(decodedCameraPage0.firmware, '12.11.77');
assert.strictEqual(decodedCameraPage0.uptimeHours, 981);
assert.strictEqual(decodedCameraPage0.cpuUsagePercent, 83);
assert.strictEqual(decodedCameraPage0.appVersionFragment, '2.');

const decodedCamera = decoder.Decode({ port: 120, messages: [cameraPage0, cameraPage1] });
assert.strictEqual(decodedCamera.model, 'Q3548-LVE');
assert.strictEqual(decodedCamera.serial, 'B8A44FF11A35');
assert.strictEqual(decodedCamera.firmware, '12.11.77');
assert.strictEqual(decodedCamera.uptimeHours, 981);
assert.strictEqual(decodedCamera.cpuUsagePercent, 83);
assert.strictEqual(decodedCamera.appVersion, '2.0.0');
assert.strictEqual(decodedCamera.page, undefined);
assert.strictEqual(decodedCamera.pageCount, undefined);

function ascii(value) {
	return Array.from(Buffer.from(value, 'ascii'));
}

const structuredCameraBody = [
	1, 0x81, 9, 12, 8, 5,
	0xD6, 0x03, 0x00, 0x00,
	...ascii('Q3548-LVE'),
	...ascii('B8A44FF11A35'),
	...ascii('12.11.77'),
	...ascii('2.0.0')
];
const structuredCameraMessage = decoder.makeFrame(0x81, 0, structuredCameraBody);
assert.strictEqual(structuredCameraMessage.length / 2, 48);
const structuredCamera = decoder.Decode(120, structuredCameraMessage);
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(structuredCamera)),
	{
		port: 120,
		command: 'get_response',
		commandCode: 129,
		transactionId: 0,
		infoType: 'cameraInfo',
		model: 'Q3548-LVE',
		serial: 'B8A44FF11A35',
		firmware: '12.11.77',
		appVersion: '2.0.0',
		uptimeHours: 982
	}
);

const structuredBridgeBody = [
	2, 0x81, 7, 3, 5, 3, 8,
	0x5F, 0x01, 20, 0,
	...ascii('AI-B100'),
	...ascii('1.3'),
	...ascii('2.1.9'),
	...ascii('usb'),
	...ascii('007B0503')
];
const structuredBridge = decoder.Decode(120, decoder.makeFrame(0x81, 0, structuredBridgeBody));
assert.strictEqual(structuredBridge.hardware, 'AI-B100');
assert.strictEqual(structuredBridge.hardwareVersion, '1.3');
assert.strictEqual(structuredBridge.firmware, '2.1.9');
assert.strictEqual(structuredBridge.powerSource, 'usb');
assert.strictEqual(structuredBridge.temperatureC, 35.1);
assert.strictEqual(structuredBridge.restartCounter, 20);
assert.strictEqual(structuredBridge.devAddr, '007B0503');

const publishCases = [
	{ name: 'counting', body: [1, 1, 5], config: { service: 'counting', active: true, intervall: 5 } },
	{ name: 'occupancy', body: [2, 0, 60], config: { service: 'occupancy', active: false, interval: 60 } },
	{ name: 'presence', body: [3, 1], config: { service: 'presence', active: true } }
];
publishCases.forEach((publishCase) => {
	const getRequest = encoder.Encode({ type: 'Publish', config: { service: publishCase.name } });
	assert.deepStrictEqual(bodyOf(getRequest.message), [publishCase.body[0]]);
	const setRequest = encoder.Encode({ type: 'Publish', config: publishCase.config });
	assert.deepStrictEqual(bodyOf(setRequest.message), publishCase.body);
	assert.deepStrictEqual(
		JSON.parse(JSON.stringify(response(130, 0x81, publishCase.body).config)),
		JSON.parse(JSON.stringify(decoder.Decode(setRequest).config))
	);
	const acknowledgement = response(130, 0x82, [publishCase.body[0], 0]);
	assert.strictEqual(acknowledgement.useCase, publishCase.name);
	assert.strictEqual(acknowledgement.statusName, 'ok');
});
assert.deepStrictEqual(
	JSON.parse(JSON.stringify(response(130, 0x83, [1, 1, 1, 60, 2, 1, 1, 60, 3, 0, 0, 0]).useCases)),
	[
		{ useCase: 'counting', intervalSupported: true, minimumIntervalMinutes: 1, maximumIntervalMinutes: 60 },
		{ useCase: 'occupancy', intervalSupported: true, minimumIntervalMinutes: 1, maximumIntervalMinutes: 60 },
		{ useCase: 'presence', intervalSupported: false, minimumIntervalMinutes: 0, maximumIntervalMinutes: 0 }
	]
);

const occupancy = encoder.Encode({
	type: 'Occupancy',
	sceneIndex: 1,
	config: {
		publishClasses: ['human', 'car'],
		valueType: 'average',
		points: points.slice(0, 4)
	}
});
assert.strictEqual(occupancy.port, 132);

const presenceScene = encoder.Encode({
	type: 'PresenceAlert',
	sceneIndex: 1,
	config: {
		classes: ['human'],
		thresholdObjectCount: 1,
		triggerDelaySeconds: 10,
		points: points.slice(0, 4)
	}
});
assert.strictEqual(presenceScene.port, 133);

[
	{ port: 131, encoded: entry, minimumPoints: 2 },
	{ port: 132, encoded: occupancy, minimumPoints: 3 },
	{ port: 133, encoded: presenceScene, minimumPoints: 3 }
].forEach((sceneCase) => {
	const sceneBody = bodyOf(sceneCase.encoded.message);
	const getRequest = encoder.Encode({ port: sceneCase.port, command: 'get', sceneIndex: 1 });
	assert.deepStrictEqual(bodyOf(getRequest.message), [1, 0]);
	assert.strictEqual(response(sceneCase.port, 0x81, sceneBody).scene.index, 1);
	const acknowledgement = response(sceneCase.port, 0x82, [1, 0]);
	assert.strictEqual(acknowledgement.sceneIndex, 1);
	assert.strictEqual(acknowledgement.statusName, 'ok');
	const capabilities = response(sceneCase.port, 0x83, [2, 10, 47, 1, 6, sceneCase.minimumPoints, 10]);
	assert.strictEqual(capabilities.configVersion, 2);
	assert.strictEqual(capabilities.minimumPoints, sceneCase.minimumPoints);
	assert.strictEqual(capabilities.maximumPoints, 10);
	assert.strictEqual(capabilities.pagingSupported, true);
	assert.deepStrictEqual(bodyOf(encoder.Encode({ port: sceneCase.port, command: 'list' }).message), [0]);
});

const errorResponse = response(130, 0xE0, [0x01, 0x03], 19);
assert.strictEqual(errorResponse.requestCommandName, 'get');
assert.strictEqual(errorResponse.statusName, 'invalidRange');
assert.strictEqual(errorResponse.transactionId, 19);

assert.throws(() => encoder.Encode({ port: 100, command: 'get' }), /GET is unsupported/);
assert.throws(() => encoder.Encode({ port: 120, command: 'set' }), /SET is unsupported/);
assert.throws(() => encoder.Encode({ port: 130, command: 'list' }), /LIST is unsupported/);
assert.throws(() => encoder.Encode({ port: 999, command: 'caps' }), /Unsupported OTA port/);

const corrupted = transmission.message.slice(0, -2) + '00';
assert.throws(() => decoder.Decode(130, corrupted), /CRC mismatch/);

assert(!extract('OTA_ENCODER_JS').includes('module.exports'));
assert(!extract('OTA_DECODER_JS').includes('module.exports'));