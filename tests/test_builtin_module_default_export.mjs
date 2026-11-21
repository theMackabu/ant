import assert from 'node:assert';
import http, * as httpNamespace from 'node:http';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
for (const name of ['http', 'https', 'fs', 'path', 'util', 'events', 'os', 'url']) {
  const exports = require('node:' + name);
  assert.strictEqual(exports, require(name), name);
  assert.ok(Object.isExtensible(exports), name);
  exports.__probe = 1;
  assert.strictEqual(exports.__probe, 1, name);
  assert.strictEqual(delete exports.__probe, true, name);
}
const required = require('node:http');
assert.strictEqual(required, http);
assert.strictEqual(httpNamespace.default, http);
assert.strictEqual(Object.isExtensible(httpNamespace), false);
const original = required.request;
Object.defineProperty(required, 'request', { get() { return original; }, configurable: true });
assert.strictEqual(required.request, original);
Object.defineProperty(required, 'request', { value: original, writable: true, enumerable: true, configurable: true });

console.log('ok');
