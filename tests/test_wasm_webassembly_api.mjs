import { readFileSync } from 'ant:fs';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function fixture(path) {
  return readFileSync(import.meta.dirname + '/../' + path);
}

const api = globalThis.WebAssembly;

assert(api && typeof api === 'object', 'globalThis.WebAssembly should exist');

for (const name of [
  'Global',
  'Instance',
  'Memory',
  'Module',
  'Table',
  'Tag',
  'Exception',
  'CompileError',
  'LinkError',
  'RuntimeError',
]) {
  assert(typeof api[name] === 'function', `WebAssembly.${name} should exist`);
}

const incrementer = fixture('wpt/wasm/webapi/resources/incrementer.wasm');

assert(WebAssembly.validate(incrementer) === true, 'incrementer.wasm should validate');

const module = new WebAssembly.Module(incrementer);
const descriptors = WebAssembly.Module.exports(module);
assert(Array.isArray(descriptors), 'WebAssembly.Module.exports() should return an array');
assert(
  descriptors.some(entry => entry && entry.name === 'increment' && entry.kind === 'function'),
  'incrementer.wasm should export increment()',
);

const instance = new WebAssembly.Instance(module);
assert(typeof instance.exports.increment === 'function', 'instance.exports.increment should exist');
assert(instance.exports.increment(41) === 42, 'increment(41) should return 42');

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
  assert(
    error instanceof WebAssembly.CompileError || error?.name === 'CompileError',
    'invalid module should throw CompileError',
  );
}
assert(sawCompileError, 'invalid module construction should fail');

console.log('wasm:webassembly-api:ok');
