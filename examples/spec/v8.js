import { test, testDeep, summary } from './helpers.js';
import v8 from 'node:v8';

console.log('startupSnapshot\n');

test('isBuildingSnapshot is a function', typeof v8.startupSnapshot.isBuildingSnapshot, 'function');
test('isBuildingSnapshot returns false', v8.startupSnapshot.isBuildingSnapshot(), false);
test('addSerializeCallback is a function', typeof v8.startupSnapshot.addSerializeCallback, 'function');
test('addDeserializeCallback is a function', typeof v8.startupSnapshot.addDeserializeCallback, 'function');
test('addSerializeCallback returns undefined', v8.startupSnapshot.addSerializeCallback(() => {}), undefined);
test('addDeserializeCallback returns undefined', v8.startupSnapshot.addDeserializeCallback(() => {}), undefined);

console.log('\ncachedDataVersionTag\n');

test('cachedDataVersionTag is a function', typeof v8.cachedDataVersionTag, 'function');
const tag = v8.cachedDataVersionTag();
test('cachedDataVersionTag returns a number', typeof tag, 'number');
test('cachedDataVersionTag is non-zero', tag !== 0, true);
test('cachedDataVersionTag is stable', v8.cachedDataVersionTag() === v8.cachedDataVersionTag(), true);

console.log('\ngetHeapStatistics\n');

const stats = v8.getHeapStatistics();
test('getHeapStatistics returns object', typeof stats, 'object');

const heapFields = [
  'total_heap_size', 'total_heap_size_executable', 'total_physical_size',
  'total_available_size', 'total_global_handles_size', 'used_global_handles_size',
  'used_heap_size', 'heap_size_limit', 'malloced_memory',
  'peak_malloced_memory', 'does_zap_garbage', 'number_of_native_contexts',
  'number_of_detached_contexts', 'total_heap_blinded_size'
];
for (const field of heapFields) {
  test(`getHeapStatistics has ${field}`, typeof stats[field], 'number');
}

test('used_heap_size > 0', stats.used_heap_size > 0, true);
test('total_heap_size >= used_heap_size', stats.total_heap_size >= stats.used_heap_size, true);
test('total_physical_size > 0', stats.total_physical_size > 0, true);
test('heap_size_limit > 0', stats.heap_size_limit > 0, true);
test('number_of_native_contexts >= 1', stats.number_of_native_contexts >= 1, true);

console.log('\ngetHeapSpaceStatistics\n');

const spaces = v8.getHeapSpaceStatistics();
test('getHeapSpaceStatistics returns array', Array.isArray(spaces), true);
test('getHeapSpaceStatistics has spaces', spaces.length >= 1, true);

const spaceFields = ['space_name', 'space_size', 'space_used_size', 'space_available_size', 'physical_space_size'];
for (const space of spaces) {
  test(`space has name`, typeof space.space_name, 'string');
  for (const field of spaceFields) {
    test(`${space.space_name} has ${field}`, typeof space[field], field === 'space_name' ? 'string' : 'number');
  }
}

console.log('\ngetHeapCodeStatistics\n');

const code = v8.getHeapCodeStatistics();
test('getHeapCodeStatistics returns object', typeof code, 'object');
test('has code_and_metadata_size', typeof code.code_and_metadata_size, 'number');
test('has bytecode_and_metadata_size', typeof code.bytecode_and_metadata_size, 'number');
test('has external_script_source_size', typeof code.external_script_source_size, 'number');
test('has cpu_profiler_metadata_size', typeof code.cpu_profiler_metadata_size, 'number');
test('code_and_metadata_size >= bytecode_and_metadata_size', code.code_and_metadata_size >= code.bytecode_and_metadata_size, true);
test('external_script_source_size > 0', code.external_script_source_size > 0, true);

console.log('\nserialize / deserialize\n');

const roundtrip = (label, value) => {
  const buf = v8.serialize(value);
  test(`${label}: serialize returns Uint8Array`, buf instanceof Uint8Array, true);
  test(`${label}: buffer is non-empty`, buf.length > 0, true);
  const out = v8.deserialize(buf);
  test(`${label}: round-trip typeof`, typeof out, typeof value);
  return out;
};

roundtrip('undefined', undefined);
roundtrip('null', null);
roundtrip('true', true);
roundtrip('false', false);

const n = roundtrip('integer', 42);
test('integer value preserved', n, 42);

const f = roundtrip('float', 3.14);
test('float value preserved', Math.abs(f - 3.14) < 1e-10, true);

const buf = v8.serialize(NaN);
const nan = v8.deserialize(buf);
test('NaN round-trips', Number.isNaN(nan), true);

const s = roundtrip('string', 'hello world');
test('string value preserved', s, 'hello world');

const empty = roundtrip('empty string', '');
test('empty string preserved', empty, '');

const arr = v8.deserialize(v8.serialize([1, 2, 3]));
test('array round-trips', Array.isArray(arr), true);
test('array length preserved', arr.length, 3);
test('array[0]', arr[0], 1);
test('array[2]', arr[2], 3);

const obj = v8.deserialize(v8.serialize({ x: 1, y: 'two', z: true }));
test('object round-trips', typeof obj, 'object');
test('object.x', obj.x, 1);
test('object.y', obj.y, 'two');
test('object.z', obj.z, true);

const nested = v8.deserialize(v8.serialize({ a: [1, { b: 2 }], c: null }));
test('nested object.a is array', Array.isArray(nested.a), true);
test('nested object.a[1].b', nested.a[1].b, 2);
test('nested object.c is null', nested.c, null);

let threw = false;
try { v8.deserialize(new Uint8Array([0x00, 0x00])); } catch (e) { threw = true; }
test('deserialize rejects bad magic', threw, true);

console.log('\nno-op methods\n');

test('setFlagsFromString is a function', typeof v8.setFlagsFromString, 'function');
test('setFlagsFromString returns undefined', v8.setFlagsFromString('--harmony'), undefined);
test('stopCoverage is a function', typeof v8.stopCoverage, 'function');
test('stopCoverage returns undefined', v8.stopCoverage(), undefined);
test('takeCoverage is a function', typeof v8.takeCoverage, 'function');
test('takeCoverage returns undefined', v8.takeCoverage(), undefined);

console.log('\nconstants\n');

test('constants is an object', typeof v8.constants, 'object');

summary();
