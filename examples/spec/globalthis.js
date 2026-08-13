import { test, summary } from './helpers.js';

console.log('GlobalThis Tests\n');

test('globalThis exists', typeof globalThis, 'object');
test('globalThis.console', typeof globalThis.console, 'object');
test('globalThis.setTimeout', typeof globalThis.setTimeout, 'function');
test('globalThis.Object', globalThis.Object === Object, true);
test('globalThis.Array', globalThis.Array === Array, true);
test('globalThis.Math', globalThis.Math === Math, true);
test('globalThis.JSON', globalThis.JSON === JSON, true);
test(
  'globalThis brand',
  Object.prototype.toString.call(globalThis),
  '[object global]'
);

function descriptorFlags(name) {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
  if (!descriptor) return 'missing';
  return `${descriptor.writable}/${descriptor.enumerable}/${descriptor.configurable}`;
}

const standardGlobals = [
  'Error',
  'EvalError',
  'RangeError',
  'ReferenceError',
  'SyntaxError',
  'TypeError',
  'URIError',
  'AggregateError',
  'Object',
  'Function',
  'String',
  'Number',
  'Boolean',
  'Array',
  'Proxy',
  'Promise',
  'Symbol',
  'Iterator',
  'ArrayBuffer',
  'Int8Array',
  'Uint8Array',
  'Uint8ClampedArray',
  'Int16Array',
  'Uint16Array',
  'Int32Array',
  'Uint32Array',
  'Float16Array',
  'Float32Array',
  'Float64Array',
  'BigInt64Array',
  'BigUint64Array',
  'DataView',
  'SharedArrayBuffer',
  'Math',
  'BigInt',
  'Date',
  'RegExp',
  'Map',
  'Set',
  'WeakMap',
  'WeakSet',
  'WeakRef',
  'FinalizationRegistry',
  'Atomics',
  'JSON',
  'Reflect',
  'Intl',
  'WebAssembly'
];

for (const name of standardGlobals) {
  test(
    `${name} global descriptor`,
    descriptorFlags(name),
    'true/false/true'
  );
}

const interfaceConstructors = [
  'DOMException',
  'AbortController',
  'AbortSignal',
  'Headers',
  'Blob',
  'File',
  'FormData',
  'Request',
  'Response',
  'Event',
  'CustomEvent',
  'ErrorEvent',
  'PromiseRejectionEvent',
  'EventTarget',
  'WebSocket',
  'MessageEvent',
  'CloseEvent',
  'URLSearchParams',
  'URL',
  'TextEncoder',
  'TextDecoder',
  'EventSource'
];

for (const name of interfaceConstructors) {
  test(
    `${name} global descriptor`,
    descriptorFlags(name),
    'true/false/true'
  );
}

for (const name of ['structuredClone', 'atob', 'btoa']) {
  test(
    `${name} global descriptor`,
    descriptorFlags(name),
    'true/true/true'
  );
}

const globalTagDescriptor = Object.getOwnPropertyDescriptor(
  globalThis,
  Symbol.toStringTag
);
test('globalThis Symbol.toStringTag value', globalTagDescriptor?.value, 'global');
test(
  'globalThis Symbol.toStringTag descriptor',
  [
    globalTagDescriptor?.writable,
    globalTagDescriptor?.enumerable,
    globalTagDescriptor?.configurable
  ].join('/'),
  'false/false/true'
);

for (const name of [
  'InternalError',
  'SuppressedError',
  'DisposableStack',
  'AsyncDisposableStack',
  'AsyncIterator',
  'Observable',
  'Buffer',
  'Stats',
  'console',
  'process',
  'Ant',
  'import',
  '__dirname',
  '__filename'
]) {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
  test(`${name} is not enumerable`, descriptor?.enumerable, false);
}

globalThis.testGlobal = 42;
test('set on globalThis', testGlobal, 42);

test('process exists', typeof process, 'object');
test('process.env exists', typeof process.env, 'object');
test('process.argv exists', typeof process.argv, 'object');
test('process.exit exists', typeof process.exit, 'function');
test('process.cwd exists', typeof process.cwd, 'function');

summary();
