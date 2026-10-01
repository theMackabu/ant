// new looks up the constructor's prototype through a per-function slot cache
// (sv_construct_prototype_cached). Replacing F.prototype, a non-object
// prototype, and alternating constructors at one site must all be observed.
const assert = require('node:assert');
function F(v) { this.v = v; }
F.prototype.get = function () { return 'a' + this.v; };
function make(C, i) { return new C(i); }
for (let i = 0; i < 20000; i++) assert.strictEqual(make(F, i).get(), 'a' + i);
F.prototype = { get() { return 'b' + this.v; } };
for (let i = 0; i < 2000; i++) assert.strictEqual(make(F, i).get(), 'b' + i);
F.prototype = 5;  // not an object: instances get Object.prototype
for (let i = 0; i < 2000; i++) assert.strictEqual(Object.getPrototypeOf(make(F, i)), Object.prototype);
function G(v) { this.v = v; }
G.prototype.get = function () { return 'g' + this.v; };
for (let i = 0; i < 2000; i++) { assert.strictEqual(make(i % 2 ? F : G, i) instanceof (i % 2 ? Object : G), true); }
console.log('PASS new follows the constructor prototype');
