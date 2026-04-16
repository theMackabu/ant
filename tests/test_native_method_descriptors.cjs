let pass = true;
const { EventEmitter } = require('events');

if ('a' + [].at !== 'afunction at() { [native code] }') {
  console.log('FAIL: built-in methods should stringify with their exposed property name');
  pass = false;
}

if (fetch.name !== 'fetch') {
  console.log('FAIL: global native functions should expose their property name');
  pass = false;
}

if (Number.parseInt !== globalThis.parseInt || Number.parseFloat !== globalThis.parseFloat) {
  console.log('FAIL: Number parse aliases should reuse the global native functions');
  pass = false;
}

if (Function.prototype.toString.call(fetch) !== 'function fetch() { [native code] }') {
  console.log('FAIL: Function.prototype.toString should use the exposed global name for native functions');
  pass = false;
}

const nameDesc = Object.getOwnPropertyDescriptor(Function.prototype.toString, 'name');
if (!nameDesc || nameDesc.value !== 'toString') {
  console.log('FAIL: Function.prototype.toString should expose a name descriptor');
  pass = false;
}

if (!nameDesc || nameDesc.writable !== false || nameDesc.enumerable !== false || nameDesc.configurable !== true) {
  console.log('FAIL: Function.prototype.toString.name descriptor flags should match builtin functions');
  pass = false;
}

const lengthDesc = Object.getOwnPropertyDescriptor(Function.prototype.toString, 'length');
if (!lengthDesc) {
  console.log('FAIL: Function.prototype.toString should expose a length descriptor');
  pass = false;
}

if (!lengthDesc || lengthDesc.writable !== false || lengthDesc.enumerable !== false || lengthDesc.configurable !== true) {
  console.log('FAIL: Function.prototype.toString.length descriptor flags should match builtin functions');
  pass = false;
}

const names = Object.getOwnPropertyNames(Function.prototype.toString);
if (!names.includes('name') || !names.includes('length')) {
  console.log('FAIL: Object.getOwnPropertyNames should include builtin function metadata');
  pass = false;
}

if (String.prototype.toLowerCase.name !== 'toLowerCase') {
  console.log('FAIL: shared native entrypoints should preserve the primary exposed name');
  pass = false;
}

if (String.prototype.toLocaleLowerCase.name !== 'toLocaleLowerCase') {
  console.log('FAIL: shared native entrypoints should preserve alternate exposed names');
  pass = false;
}

if (URLSearchParams.prototype[Symbol.iterator] !== URLSearchParams.prototype.entries) {
  console.log('FAIL: URLSearchParams @@iterator should alias entries');
  pass = false;
}

if (URLSearchParams.prototype[Symbol.iterator].name !== 'entries') {
  console.log('FAIL: URLSearchParams @@iterator should keep the entries name');
  pass = false;
}

if (Headers.prototype[Symbol.iterator] !== Headers.prototype.entries) {
  console.log('FAIL: Headers @@iterator should alias entries');
  pass = false;
}

if (Headers.prototype[Symbol.iterator].name !== 'entries') {
  console.log('FAIL: Headers @@iterator should keep the entries name');
  pass = false;
}

if (Map.prototype[Symbol.iterator] !== Map.prototype.entries) {
  console.log('FAIL: Map @@iterator should alias entries');
  pass = false;
}

if (Map.prototype[Symbol.iterator].name !== 'entries') {
  console.log('FAIL: Map @@iterator should keep the entries name');
  pass = false;
}

if (Set.prototype.keys !== Set.prototype.values || Set.prototype[Symbol.iterator] !== Set.prototype.values) {
  console.log('FAIL: Set iterator aliases should reuse values');
  pass = false;
}

if (Set.prototype[Symbol.iterator].name !== 'values') {
  console.log('FAIL: Set @@iterator should keep the values name');
  pass = false;
}

if (FormData.prototype[Symbol.iterator] !== FormData.prototype.entries) {
  console.log('FAIL: FormData @@iterator should alias entries');
  pass = false;
}

if (EventEmitter.prototype.on !== EventEmitter.prototype.addListener) {
  console.log('FAIL: EventEmitter.addListener should alias on');
  pass = false;
}

if (EventEmitter.prototype.off !== EventEmitter.prototype.removeListener) {
  console.log('FAIL: EventEmitter.removeListener should alias off');
  pass = false;
}

if (process.on !== process.addListener || process.off !== process.removeListener) {
  console.log('FAIL: process event aliases should reuse the canonical methods');
  pass = false;
}

if (
  process.stdin.off !== process.stdin.removeListener ||
  process.stdout.off !== process.stdout.removeListener ||
  process.stderr.off !== process.stderr.removeListener
) {
  console.log('FAIL: stdio removeListener aliases should reuse the canonical methods');
  pass = false;
}

if (Uint8Array.prototype[Symbol.iterator] !== Uint8Array.prototype.values) {
  console.log('FAIL: TypedArray @@iterator should alias values');
  pass = false;
}

if (Uint8Array.prototype[Symbol.iterator].name !== 'values') {
  console.log('FAIL: TypedArray @@iterator should keep the values name');
  pass = false;
}

const wrapped = (() => {}).bind(null);
Object.defineProperty(wrapped, 'name', nameDesc);

if (wrapped.name !== 'toString') {
  console.log('FAIL: builtin function name descriptors should round-trip through defineProperty');
  pass = false;
}

if (pass) console.log('PASS');
