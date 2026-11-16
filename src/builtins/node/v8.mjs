import binding from 'ant:internal/v8';
import { Buffer } from 'node:buffer';
import { inspect } from 'node:util';

const { Serializer, Deserializer, getHeapStatistics, getHeapSpaceStatistics, getHeapCodeStatistics } = binding;

const BUFFER_INDEX = 10;

const VIEW_TYPES = [
  Int8Array,
  Uint8Array,
  Uint8ClampedArray,
  Int16Array,
  Uint16Array,
  Int32Array,
  Uint32Array,
  Float32Array,
  Float64Array,
  DataView,
  Buffer,
  BigInt64Array,
  BigUint64Array,
  Float16Array
];

Serializer.prototype._getDataCloneError = Error;

Deserializer.prototype.readRawBytes = function readRawBytes(length) {
  const offset = this._readRawBytes(length);
  return Buffer.from(this.buffer.buffer, this.buffer.byteOffset + offset, length);
};

class DefaultSerializer extends Serializer {
  constructor() {
    super();
    this._setTreatArrayBufferViewsAsHostObjects(true);
  }

  _writeHostObject(view) {
    const index = view.constructor === Buffer ? BUFFER_INDEX : VIEW_TYPES.findIndex(type => type !== Buffer && view instanceof type);
    if (index === -1) throw new this._getDataCloneError(`Unserializable host object: ${inspect(view)}`);
    this.writeUint32(index);
    this.writeUint32(view.byteLength);
    this.writeRawBytes(new Uint8Array(view.buffer, view.byteOffset, view.byteLength));
  }
}

class DefaultDeserializer extends Deserializer {
  _readHostObject() {
    const index = this.readUint32();
    const type = VIEW_TYPES[index];
    if (type === undefined) throw new Error(`Unknown host object type: ${index}`);

    const byteLength = this.readUint32();
    const size = type.BYTES_PER_ELEMENT || 1;
    
    let buffer = this.buffer.buffer;
    let offset = this.buffer.byteOffset + this._readRawBytes(byteLength);
    
    if (offset % size !== 0) {
      buffer = buffer.slice(offset, offset + byteLength);
      offset = 0;
    }

    if (index === BUFFER_INDEX) return Buffer.from(buffer, offset, byteLength);
    return new type(buffer, offset, byteLength / size);
  }
}

function serialize(value) {
  const serializer = new DefaultSerializer();
  serializer.writeHeader();
  serializer.writeValue(value);
  return serializer.releaseBuffer();
}

function deserialize(buffer) {
  const deserializer = new DefaultDeserializer(buffer);
  deserializer.readHeader();
  return deserializer.readValue();
}

const versionTag = Number(Ant.raw.hash(Ant.version) & 0xffffffffn);

function cachedDataVersionTag() {
  return versionTag;
}

function writeHeapSnapshot() {
  return '';
}

function queryObjects(constructor, options) {
  return options?.format === 'summary' ? [] : 0;
}

function getHeapSnapshot() {}
function setFlagsFromString() {}

function takeCoverage() {}
function stopCoverage() {}

function setHeapSnapshotNearHeapLimit() {}
function setHeapProfileNearHeapLimit() {}

const getCppHeapStatistics =() => ({
  committed_size_bytes: 0,
  resident_size_bytes: 0,
  used_size_bytes: 0,
  space_statistics: [],
  type_names: []
});

function isStringOneByteRepresentation(content) {
  if (typeof content !== 'string') throw new TypeError('The "content" argument must be of type string.');
  for (let i = 0; i < content.length; i++) if (content.charCodeAt(i) > 0xff) return false;
  return true;
}

function startCpuProfile() {
  return { stop: () => JSON.stringify({ nodes: [], startTime: 0, endTime: 0, samples: [], timeDeltas: [] }) };
}

function startHeapProfile() {
  return { stop: () => JSON.stringify({ head: { callFrame: {}, selfSize: 0, id: 1, children: [] }, samples: [] }) };
}

class GCProfiler {
  #startTime = 0;

  start() {
    this.#startTime ||= Date.now();
  }

  stop() {
    if (!this.#startTime) return undefined;
    const report = { version: 1, startTime: this.#startTime, statistics: [], endTime: Date.now() };
    this.#startTime = 0;
    return report;
  }
}

const stopHook = () => {};
const promiseHooks = {
  createHook: () => stopHook,
  onInit: () => stopHook,
  onBefore: () => stopHook,
  onAfter: () => stopHook,
  onSettled: () => stopHook
};

const startupSnapshot = {
  addDeserializeCallback() {},
  addSerializeCallback() {},
  setDeserializeMainFunction() {},
  isBuildingSnapshot: () => false
};

export {
  cachedDataVersionTag,
  getHeapSnapshot,
  getHeapStatistics,
  getHeapSpaceStatistics,
  getHeapCodeStatistics,
  getCppHeapStatistics,
  setFlagsFromString,
  Serializer,
  Deserializer,
  DefaultSerializer,
  DefaultDeserializer,
  deserialize,
  takeCoverage,
  stopCoverage,
  serialize,
  writeHeapSnapshot,
  promiseHooks,
  queryObjects,
  startupSnapshot,
  setHeapSnapshotNearHeapLimit,
  setHeapProfileNearHeapLimit,
  GCProfiler,
  isStringOneByteRepresentation,
  startCpuProfile,
  startHeapProfile
};

export default {
  cachedDataVersionTag,
  getHeapSnapshot,
  getHeapStatistics,
  getHeapSpaceStatistics,
  getHeapCodeStatistics,
  getCppHeapStatistics,
  setFlagsFromString,
  Serializer,
  Deserializer,
  DefaultSerializer,
  DefaultDeserializer,
  deserialize,
  takeCoverage,
  stopCoverage,
  serialize,
  writeHeapSnapshot,
  promiseHooks,
  queryObjects,
  startupSnapshot,
  setHeapSnapshotNearHeapLimit,
  setHeapProfileNearHeapLimit,
  GCProfiler,
  isStringOneByteRepresentation,
  startCpuProfile,
  startHeapProfile
};
