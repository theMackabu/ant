import { alloc, callback, dlopen, pointer, suffix, FFIType } from 'ant:ffi';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function expectThrow(fn, message) {
  let threw = false;
  try {
    fn();
  } catch (_) {
    threw = true;
  }
  assert(threw, message);
}

let libcName = `libc.${suffix}`;
if (process.platform === 'darwin') libcName = 'libSystem.dylib';
if (process.platform === 'linux') libcName = 'libc.so.6';
if (process.platform === 'win32') libcName = 'msvcrt.dll';

const libc = dlopen(libcName);
const abs = libc.define('abs', {
  args: [FFIType.int],
  returns: FFIType.int
});

assert(typeof abs === 'function', 'define() should return a callable FFIFunction');
assert(abs.address() > 0, 'FFIFunction.address() should expose a native address');
assert(abs(-7) === 7, 'FFIFunction should be callable directly');
assert(libc.call('abs', -9) === 9, 'FFILibrary.call() should dispatch through defined wrappers');

const strlen = libc.define('strlen', {
  args: [FFIType.string],
  returns: FFIType.int
});
assert(strlen('planet') === 6, 'string arguments should support borrowed JS strings for call-only use');

const strchr = libc.define('strchr', {
  args: [FFIType.string, FFIType.int],
  returns: FFIType.pointer
});

const haystack = pointer('planet');
const tail = strchr(haystack, 'n'.charCodeAt(0));
assert(tail && typeof tail.read === 'function', 'pointer returns should materialize FFIPointer objects');
assert(tail.read(FFIType.string) === 'net', 'FFIPointer.read(string) should decode C strings');
assert(strchr(haystack, 'z'.charCodeAt(0)) === null, 'null pointer returns should become null');

const owned = pointer('hello ffi');
assert(owned.read(FFIType.string) === 'hello ffi', 'pointer(string) should allocate an owned C string');
owned.free();
expectThrow(() => owned.read(FFIType.string), 'reading a freed pointer should throw');

const slot = alloc(8);
const namePtr = pointer('ant');
slot.write(FFIType.pointer, namePtr);
const roundTrip = slot.read(FFIType.pointer);
assert(roundTrip.read(FFIType.string) === 'ant', 'pointer slots should round-trip FFIPointer values');

const values = alloc(16);
values.offset(0).write(FFIType.int, 4);
values.offset(4).write(FFIType.int, 1);
values.offset(8).write(FFIType.int, 3);
values.offset(12).write(FFIType.int, 2);

const qsort = libc.define('qsort', {
  args: [FFIType.pointer, FFIType.int, FFIType.int, FFIType.pointer],
  returns: FFIType.void
});

const compareInts = callback(
  {
    args: [FFIType.pointer, FFIType.pointer],
    returns: FFIType.int
  },
  (left, right) => left.read(FFIType.int) - right.read(FFIType.int)
);

qsort(values, 4, 4, compareInts);
assert(values.offset(0).read(FFIType.int) === 1, 'qsort should sort the first element');
assert(values.offset(4).read(FFIType.int) === 2, 'qsort should sort the second element');
assert(values.offset(8).read(FFIType.int) === 3, 'qsort should sort the third element');
assert(values.offset(12).read(FFIType.int) === 4, 'qsort should sort the fourth element');

compareInts.close();
expectThrow(() => libc.define('abs', { args: [FFIType.spread, FFIType.int], returns: FFIType.int }), 'invalid spread placement should throw');

const closable = dlopen(libcName);
const closableAbs = closable.define('abs', {
  args: [FFIType.int],
  returns: FFIType.int
});
closable.close();
expectThrow(() => closableAbs(-1), 'FFIFunctions should fail once their library is closed');

slot.free();
namePtr.free();
values.free();
libc.close();
haystack.free();

console.log('ffi-wrappers:ok');
