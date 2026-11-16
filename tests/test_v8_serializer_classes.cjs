const assert = require('node:assert');
const v8 = require('node:v8');

assert.deepStrictEqual(Object.keys(v8), [
  'cachedDataVersionTag',
  'getHeapSnapshot',
  'getHeapStatistics',
  'getHeapSpaceStatistics',
  'getHeapCodeStatistics',
  'getCppHeapStatistics',
  'setFlagsFromString',
  'Serializer',
  'Deserializer',
  'DefaultSerializer',
  'DefaultDeserializer',
  'deserialize',
  'takeCoverage',
  'stopCoverage',
  'serialize',
  'writeHeapSnapshot',
  'promiseHooks',
  'queryObjects',
  'startupSnapshot',
  'setHeapSnapshotNearHeapLimit',
  'setHeapProfileNearHeapLimit',
  'GCProfiler',
  'isStringOneByteRepresentation',
  'startCpuProfile',
  'startHeapProfile'
]);
assert.strictEqual(Object.getPrototypeOf(v8), Object.prototype);
assert.deepStrictEqual(Object.keys(v8.promiseHooks), ['createHook', 'onInit', 'onBefore', 'onAfter', 'onSettled']);
assert.ok(!v8.startupSnapshot.isBuildingSnapshot());
assert.strictEqual(typeof v8.startupSnapshot.setDeserializeMainFunction, 'function');
assert.strictEqual(v8.isStringOneByteRepresentation('héllo'), true);
assert.strictEqual(v8.isStringOneByteRepresentation('h€llo'), false);

const plain = new v8.Serializer();
plain.writeHeader();
assert.strictEqual(plain.writeValue({ view: new Uint8Array([1, 2, 3]).subarray(1) }), true);
plain.writeUint32(300);
plain.writeUint64(1, 2);
plain.writeDouble(1.5);
plain.writeRawBytes(Buffer.from('ok'));
const plainBytes = plain.releaseBuffer();
assert.ok(Buffer.isBuffer(plainBytes));
assert.strictEqual(
  plainBytes.toString('hex'),
  'ff0f6f220476696577420301020356420102007b01ac028280808010000000000000f83f6f6b'
);

const reader = new v8.Deserializer(plainBytes);
assert.strictEqual(reader.buffer, plainBytes);
assert.strictEqual(reader.readHeader(), true);
assert.strictEqual(reader.getWireFormatVersion(), 15);
const viewBack = reader.readValue().view;
assert.ok(viewBack instanceof Uint8Array);
assert.deepStrictEqual([...viewBack], [2, 3]);
assert.strictEqual(viewBack.buffer.byteLength, 3);
assert.strictEqual(reader.readUint32(), 300);
assert.deepStrictEqual(reader.readUint64(), [1, 2]);
assert.strictEqual(reader.readDouble(), 1.5);
assert.strictEqual(reader.readRawBytes(2).toString(), 'ok');
assert.throws(() => reader.readUint32(), /ReadUint32\(\) failed/);

const sharedBuffer = new Uint8Array([5, 6, 7, 8]).buffer;
const twoViews = new v8.Serializer();
twoViews.writeHeader();
twoViews.writeValue([new Uint8Array(sharedBuffer, 0, 2), new Uint16Array(sharedBuffer, 2, 1)]);
const twoBack = new v8.Deserializer(twoViews.releaseBuffer());
twoBack.readHeader();
const [first, second] = twoBack.readValue();
assert.strictEqual(first.buffer, second.buffer);
assert.strictEqual(second[0], new Uint16Array(sharedBuffer, 2, 1)[0]);

class Point {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
}

class PointSerializer extends v8.DefaultSerializer {
  _writeHostObject(object) {
    if (!(object instanceof Uint16Array)) return super._writeHostObject(object);
    this.writeUint32(99);
    this.writeUint32(object[0]);
    this.writeUint32(object[1]);
  }
}

class PointDeserializer extends v8.DefaultDeserializer {
  _readHostObject() {
    const offset = this.readUint32();
    if (offset !== 99) throw new Error('unexpected host object');
    return new Point(this.readUint32(), this.readUint32());
  }
}

const custom = new PointSerializer();
custom.writeHeader();
const pair = new Uint16Array([3, 4]);
custom.writeValue({ a: pair, b: pair });
const customBack = new PointDeserializer(custom.releaseBuffer());
customBack.readHeader();
const points = customBack.readValue();
assert.ok(points.a instanceof Point);
assert.strictEqual(points.a, points.b);
assert.deepStrictEqual({ ...points.a }, { x: 3, y: 4 });

const transferred = new Uint8Array([9, 9]).buffer;
const sender = new v8.Serializer();
sender.writeHeader();
sender.transferArrayBuffer(7, transferred);
sender.writeValue({ data: transferred });
const receiver = new v8.Deserializer(sender.releaseBuffer());
receiver.transferArrayBuffer(7, transferred);
receiver.readHeader();
assert.strictEqual(receiver.readValue().data, transferred);

class CloneError extends Error {}
const strict = new v8.Serializer();
strict._getDataCloneError = message => new CloneError(message);
strict.writeHeader();
assert.throws(() => strict.writeValue(() => {}), CloneError);
assert.throws(() => new v8.Serializer().writeValue(new SharedArrayBuffer(1)), /could not be cloned/);
assert.throws(() => new v8.Deserializer('nope'), TypeError);
assert.throws(() => v8.Serializer(), TypeError);

const profiler = new v8.GCProfiler();
profiler.start();
assert.strictEqual(profiler.stop().version, 1);

console.log('v8 serializer classes OK');
