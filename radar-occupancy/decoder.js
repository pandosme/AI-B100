/**
 * AI-B100 Radar Occupancy Decoder
 * Device  : D2110-VE  (serial ACCC8EF33E99)
 * Generated: 2026-06-22
 *
 * Payload format
 * --------------
 * byte 0     protocol version, currently 1
 * byte 1     mode: 0=Interval peak, 1=Area balance, 2=Presence alert
 * bytes 2-3  total occupancy, uint16 little-endian
 * bytes 4-5  human occupancy, uint16 little-endian
 * bytes 6-7  vehicle occupancy, uint16 little-endian
 * bytes 8-9  unknown occupancy, uint16 little-endian, currently always 0
 *
 * Mode semantics
 * --------------
 * Interval peak:   highest count observed during the publish interval.
 * Area balance:    current balanced inside-area count from entry/exit transitions.
 * Presence alert:  current count when a new detection episode starts; no interval publish.
 *
 */

function decodeRadarOccupancy(bytes) {
  if (!bytes || bytes.length < 10) {
    throw new Error('Radar occupancy payload must be 10 bytes');
  }
  var modeInfo = {
    0: {
      name: 'maximum',
      label: 'Interval peak',
      description: 'Highest count observed during the publish interval.'
    },
    1: {
      name: 'entry_exit',
      label: 'Area balance',
      description: 'Current balanced inside-area count from entry/exit transitions.'
    },
    2: {
      name: 'alert',
      label: 'Presence alert',
      description: 'Current count when a new detection episode starts; no interval publish.'
    }
  };
  function u16(offset) { return bytes[offset] | (bytes[offset + 1] << 8); }
  var modeCode = bytes[1];
  var mode = modeInfo[modeCode] || {
    name: 'unknown',
    label: 'Unknown',
    description: 'Unknown radar occupancy mode.'
  };
  var occupancy = {
    total: u16(2),
    human: u16(4),
    vehicle: u16(6),
    unknown: u16(8)
  };
  return {
    version: bytes[0],
    modeCode: modeCode,
    mode: mode.name,
    modeLabel: mode.label,
    modeDescription: mode.description,
    total: occupancy.total,
    human: occupancy.human,
    vehicle: occupancy.vehicle,
    unknown: occupancy.unknown,
    occupancy: occupancy,
    layout: {
      byte0: 'protocol version',
      byte1: 'mode: 0=Interval peak, 1=Area balance, 2=Presence alert',
      bytes2to3: 'total occupancy, uint16 little-endian',
      bytes4to5: 'human occupancy, uint16 little-endian',
      bytes6to7: 'vehicle occupancy, uint16 little-endian',
      bytes8to9: 'unknown occupancy, uint16 little-endian'
    }
  };
}

function decodeUplink(input) {
  try {
    return { data: decodeRadarOccupancy(input.bytes) };
  } catch (error) {
    return { errors: [error.message] };
  }
}
