const assert = require('node:assert');

class Stamp { constructor(target) { return target; } }

class Solo { #only = 1; read() { return this.#only; } write(v) { this.#only = v; } static has(o) { return #only in o; } }
class Pair { #a = 1; #b = 2; sum() { return this.#a + this.#b; } set(a, b) { this.#a = a; this.#b = b; } static has(o) { return #a in o && #b in o; } }
class Many { #a = 1; #b = 2; #c = 3; #d = 4; #e = 5; #f = 6; #g = 7; sum() { return this.#a + this.#b + this.#c + this.#d + this.#e + this.#f + this.#g; } bump() { this.#g++; } }
class Methods { #m() { return 'm'; } #n() { return 'n'; } both() { return this.#m() + this.#n(); } static has(o) { return #m in o; } }
class MethodAndField { #f = 4; #m() { return this.#f * 2; } run() { return this.#m(); } }
class Accessor { #v = 3; get #x() { return this.#v; } set #x(v) { this.#v = v; } bump() { this.#x += 1; return this.#x; } }
class SoloAccessor { get #x() { return 9; } read() { return this.#x; } }
class StaticSolo { static #count = 0; static next() { return ++StaticSolo.#count; } }
class StaticPair { static #a = 1; static #b = 2; static sum() { return StaticPair.#a + StaticPair.#b; } }

class SoloStamp extends Stamp { #s = 'solo'; static read(o) { return o.#s; } static write(o, v) { o.#s = v; } static has(o) { return #s in o; } }
class OtherSoloStamp extends Stamp { #t = 'other'; static read(o) { return o.#t; } static has(o) { return #t in o; } }
class PairStamp extends Stamp { #p = 'p'; #q = 'q'; static read(o) { return o.#p + o.#q; } static has(o) { return #p in o; } }
class BrandStamp extends Stamp { #m() { return 'brand'; } static call(o) { return o.#m(); } static has(o) { return #m in o; } }

function hosts() {
  return [
    {}, [], function () {}, new Uint8Array(4), new Proxy({}, {}), new Map(), new Date(0), /x/, new Error('host'),
    Object.freeze({}), Object.preventExtensions([]), new Solo(), new Pair(), Promise.resolve(1), new (class extends Array {})(),
  ];
}

for (let round = 0; round < 400; round++) {
  const solo = new Solo();
  assert.strictEqual(solo.read(), 1);
  solo.write(round);
  assert.strictEqual(solo.read(), round);
  assert.ok(Solo.has(solo));
  assert.ok(!Pair.has(solo));

  const pair = new Pair();
  pair.set(round, 2);
  assert.strictEqual(pair.sum(), round + 2);
  assert.ok(!Solo.has(pair));
  assert.throws(() => Solo.prototype.read.call(pair), TypeError);
  assert.throws(() => Pair.prototype.sum.call(solo), TypeError);

  const many = new Many();
  many.bump();
  assert.strictEqual(many.sum(), 29);
  assert.strictEqual(new Methods().both(), 'mn');
  assert.ok(Methods.has(new Methods()));
  assert.strictEqual(new MethodAndField().run(), 8);
  assert.strictEqual(new Accessor().bump(), 4);
  assert.strictEqual(new SoloAccessor().read(), 9);
  assert.strictEqual(StaticSolo.next(), round + 1);
  assert.strictEqual(StaticPair.sum(), 3);
}

for (const order of [[SoloStamp, OtherSoloStamp, PairStamp, BrandStamp], [PairStamp, SoloStamp, BrandStamp, OtherSoloStamp], [BrandStamp, OtherSoloStamp, SoloStamp, PairStamp]]) {
  for (const host of hosts()) {
    for (let i = 0; i < order.length; i++) {
      assert.strictEqual(new order[i](host), host);
      for (let k = 0; k < order.length; k++) assert.strictEqual(order[k].has(host), k <= i);
      assert.throws(() => new order[i](host), TypeError);
    }
    assert.strictEqual(SoloStamp.read(host), 'solo');
    SoloStamp.write(host, { kept: true });
    assert.deepStrictEqual(SoloStamp.read(host), { kept: true });
    assert.strictEqual(OtherSoloStamp.read(host), 'other');
    assert.strictEqual(PairStamp.read(host), 'pq');
    assert.strictEqual(BrandStamp.call(host), 'brand');
  }
}

{
  const kept = [];
  for (let i = 0; i < 20000; i++) {
    const host = i % 3 === 0 ? new Uint8Array(1) : i % 3 === 1 ? {} : [];
    new SoloStamp(host);
    SoloStamp.write(host, { i });
    if (i % 2) new PairStamp(host);
    if (i % 5 === 0) new OtherSoloStamp(host);
    kept.push(host);
    let junk = [];
    for (let j = 0; j < 8; j++) junk.push({ j });
  }
  for (let i = 0; i < kept.length; i++) {
    assert.strictEqual(SoloStamp.read(kept[i]).i, i);
    assert.strictEqual(PairStamp.has(kept[i]), i % 2 === 1);
    assert.strictEqual(OtherSoloStamp.has(kept[i]), i % 5 === 0);
  }
}

console.log('ok');
