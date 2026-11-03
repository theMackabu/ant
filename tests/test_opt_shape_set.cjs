const assert = require('node:assert');

function Node(key) { this.key = key; }
Node.prototype.left = null;
Node.prototype.right = null;

function makeLeftFirst(key) { const n = new Node(key); n.left = null; n.right = null; return n; }
function makeRightFirst(key) { const n = new Node(key); n.right = null; n.left = null; return n; }

function readKey(n) { return n.key; }
function writeKey(n, v) { n.key = v; return n; }
function sumKeys(a, b) { return a.key + b.key; }

const pool = [];
for (let i = 0; i < 64; i++) pool.push(i & 1 ? makeLeftFirst(i) : makeRightFirst(i));
pool.push(new Node(64));

let total = 0;
for (let r = 0; r < 4000; r++) {
  for (let i = 0; i < pool.length; i++) {
    total += readKey(pool[i]);
    writeKey(pool[i], pool[i].key);
    total += sumKeys(pool[i], pool[(i + 1) % pool.length]);
  }
}
assert.ok(total > 0);

assert.strictEqual(readKey(makeLeftFirst(7)), 7);
assert.strictEqual(readKey(makeRightFirst(8)), 8);
assert.strictEqual(readKey({ other: 1, key: 9 }), 9);
assert.strictEqual(readKey(Object.create({ key: 10 })), 10);
assert.strictEqual(readKey(new Proxy({ key: 0 }, { get: () => 11 })), 11);

const frozen = Object.freeze(makeLeftFirst(12));
assert.throws(() => { 'use strict'; writeKey(frozen, 0); writeKeyStrict(frozen, 0); });
function writeKeyStrict(n, v) { 'use strict'; n.key = v; }
assert.strictEqual(frozen.key, 12);

const odd = { a: 1, b: 2, key: 3 };
writeKey(odd, 30);
assert.strictEqual(odd.key, 30);
assert.strictEqual(sumKeys(odd, makeRightFirst(4)), 34);

const viaProxy = new Proxy({ key: 1 }, { set(t, k, v) { t[k] = v * 2; return true; } });
writeKey(viaProxy, 5);
assert.strictEqual(viaProxy.key, 10);

console.log('ok');
