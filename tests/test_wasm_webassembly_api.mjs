import { readFileSync } from 'ant:fs';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function fixture(path) {
  return readFileSync(import.meta.dirname + '/../' + path);
}

const api = globalThis.WebAssembly;

assert(api && typeof api === 'object', 'globalThis.WebAssembly should exist');

for (const name of ['Global', 'Instance', 'Memory', 'Module', 'Table', 'Tag', 'Exception', 'CompileError', 'LinkError', 'RuntimeError']) {
  assert(typeof api[name] === 'function', `WebAssembly.${name} should exist`);
}

const incrementer = fixture('wpt/wasm/webapi/resources/incrementer.wasm');
const tableExportFixture = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0, 1, 9, 2, 96, 0, 1, 127, 96, 0, 1, 127, 3, 3, 2, 0, 1, 4, 4, 1, 112, 0, 2, 7, 24, 3, 5, 115, 101, 118, 101, 110, 0, 0,
  4, 110, 105, 110, 101, 0, 1, 5, 116, 97, 98, 108, 101, 1, 0, 9, 8, 1, 0, 65, 0, 11, 2, 0, 1, 10, 11, 2, 4, 0, 65, 7, 11, 4, 0, 65, 9, 11, 0, 21, 4,
  110, 97, 109, 101, 1, 14, 2, 0, 5, 115, 101, 118, 101, 110, 1, 4, 110, 105, 110, 101
]);
const throwingImportFixture = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 2, 12, 1, 3, 101, 110, 118, 4, 102, 97, 105, 108, 0, 0, 3, 2, 1, 0, 7, 7, 1, 3, 114, 117, 110, 0, 1,
  10, 6, 1, 4, 0, 16, 0, 11
]);

assert(WebAssembly.validate(incrementer) === true, 'incrementer.wasm should validate');

const module = new WebAssembly.Module(incrementer);
const descriptors = WebAssembly.Module.exports(module);
assert(Array.isArray(descriptors), 'WebAssembly.Module.exports() should return an array');
assert(
  descriptors.some(entry => entry && entry.name === 'increment' && entry.kind === 'function'),
  'incrementer.wasm should export increment()'
);

const instance = new WebAssembly.Instance(module);
assert(typeof instance.exports.increment === 'function', 'instance.exports.increment should exist');
assert(instance.exports.increment(41) === 42, 'increment(41) should return 42');

const tableModule = new WebAssembly.Module(tableExportFixture);
const tableInstance = new WebAssembly.Instance(tableModule);
assert(tableInstance.exports.table.length === 2, 'exported table should expose its length');
const firstTableEntry = tableInstance.exports.table.get(0);
const secondTableEntry = tableInstance.exports.table.get(1);
assert(typeof firstTableEntry === 'function', 'table.get(0) should return a callable function');
assert(typeof secondTableEntry === 'function', 'table.get(1) should return a callable function');
assert(firstTableEntry() === 7, 'table.get(0) should invoke the first table function');
assert(secondTableEntry() === 9, 'table.get(1) should invoke the second table function');
tableInstance.exports.table.set(0, tableInstance.exports.nine);
assert(tableInstance.exports.table.get(0)() === 9, 'table.set() should accept wasm-exported functions');

const throwingModule = new WebAssembly.Module(throwingImportFixture);

let sawPrimitiveThrow = false;
try {
  new WebAssembly.Instance(throwingModule, {
    env: {
      fail() {
        throw Infinity;
      }
    }
  }).exports.run();
} catch (error) {
  sawPrimitiveThrow = true;
  assert(error === Infinity, 'import throws should preserve primitive thrown values');
}
assert(sawPrimitiveThrow, 'primitive import throws should escape the wasm call');

const thrownError = new Error('import boom');
let sawErrorThrow = false;
try {
  new WebAssembly.Instance(throwingModule, {
    env: {
      fail() {
        throw thrownError;
      }
    }
  }).exports.run();
} catch (error) {
  sawErrorThrow = true;
  assert(error === thrownError, 'import throws should preserve original Error objects');
}
assert(sawErrorThrow, 'Error import throws should escape the wasm call');

const thrownStatus = {
  name: 'ExitStatus',
  message: 'Program terminated with exit(1)',
  status: 1
};
let sawErrorlikeThrow = false;
try {
  new WebAssembly.Instance(throwingModule, {
    env: {
      fail() {
        throw thrownStatus;
      }
    }
  }).exports.run();
} catch (error) {
  sawErrorlikeThrow = true;
  assert(error === thrownStatus, 'import throws should preserve original error-like objects');
  assert(error.stack === undefined, 'error-like import throws should not grow a synthetic stack');
}
assert(sawErrorlikeThrow, 'error-like import throws should escape the wasm call');

const compiled = await WebAssembly.compile(incrementer);
assert(compiled instanceof WebAssembly.Module, 'WebAssembly.compile() should resolve a module');

const instantiated = await WebAssembly.instantiate(incrementer);
assert(instantiated.module instanceof WebAssembly.Module, 'instantiate(bytes) should return a module');
assert(instantiated.instance instanceof WebAssembly.Instance, 'instantiate(bytes) should return an instance');
assert(instantiated.instance.exports.increment(9) === 10, 'instantiate(bytes) should wire exports');

const global = new WebAssembly.Global({ value: 'i32', mutable: true }, 7);
assert(global.value === 7, 'WebAssembly.Global should expose its initial value');
global.value = 11;
assert(global.value === 11, 'WebAssembly.Global should accept writes');
assert(global.valueOf() === 11, 'WebAssembly.Global.prototype.valueOf() should read the value');

const invalid = new Uint8Array([0x00, 0x61, 0x62, 0x63]);
assert(WebAssembly.validate(invalid) === false, 'invalid wasm bytes should fail validation');

let sawCompileError = false;
try {
  new WebAssembly.Module(invalid);
} catch (error) {
  sawCompileError = true;
  assert(error instanceof WebAssembly.CompileError || error?.name === 'CompileError', 'invalid module should throw CompileError');
}
assert(sawCompileError, 'invalid module construction should fail');

console.log('wasm:webassembly-api:ok');
