const assert = require('node:assert');

class Ret { constructor(o) { return o; } }

{
  const order = [];
  class Pair {
    seen = (order.push('field'), [typeof this.#a, typeof this.#b, this.#g, #s in this]);
    #a() { return 'a'; }
    #b() { return 'b' + this.#a(); }
    get #g() { return 'g'; }
    set #s(v) { this.stored = v; }
    get #both() { return this.stored; }
    set #both(v) { this.stored = v * 2; }
    static has(o) { return #a in o && #b in o && #g in o && #s in o && #both in o; }
    run() { this.#s = 1; this.#both = 4; return [this.#a(), this.#b(), this.#g, this.#both]; }
    methods() { return [this.#a, this.#b]; }
    writeMethod() { this.#a = 1; }
    writeGetter() { this.#g = 1; }
    readSetter() { return this.#s; }
  }
  const p = new Pair();
  const q = new Pair();
  assert.deepStrictEqual(p.seen, ['function', 'function', 'g', true]);
  assert.deepStrictEqual(p.run(), ['a', 'ba', 'g', 8]);
  assert.strictEqual(p.methods()[0], q.methods()[0]);
  assert.strictEqual(p.methods()[1], q.methods()[1]);
  assert.deepStrictEqual(p.methods().map(f => f.name), ['#a', '#b']);
  assert.ok(Pair.has(p) && !Pair.has({}) && !Pair.has(Pair.prototype));
  assert.throws(() => p.writeMethod(), TypeError);
  assert.throws(() => p.writeGetter(), TypeError);
  assert.throws(() => p.readSetter(), TypeError);
  assert.throws(() => Pair.prototype.run.call({}), TypeError);
}

{
  class Stamp extends Ret {
    #a() { return 1; }
    #b() { return 2; }
    get #c() { return 3; }
    #f = 4;
    static sum(o) { return o.#a() + o.#b() + o.#c + o.#f; }
    static has(o) { return [#a in o, #b in o, #c in o, #f in o]; }
  }
  const target = {};
  new Stamp(target);
  assert.strictEqual(Stamp.sum(target), 10);
  assert.throws(() => new Stamp(target), TypeError);
  assert.deepStrictEqual(Stamp.has(target), [true, true, true, true]);

  class Other extends Ret { #a() { return 'x'; } #b() { return 'y'; } static read(o) { return o.#a() + o.#b(); } }
  new Other(target);
  assert.strictEqual(Other.read(target), 'xy');
  assert.strictEqual(Stamp.sum(target), 10);
  assert.throws(() => new Other(target), TypeError);

  class Half extends Ret { #a() {} #b() {} #z = (() => { throw new RangeError('stop'); })(); static has(o) { return [#a in o, #b in o, #z in o]; } }
  const partial = {};
  assert.throws(() => new Half(partial), RangeError);
  assert.deepStrictEqual(Half.has(partial), [true, true, false]);
  assert.throws(() => new Half(partial), TypeError);

  const frozen = Object.freeze({});
  new Other(frozen);
  assert.strictEqual(Other.read(frozen), 'xy');
  const proxy = new Proxy({}, { defineProperty() { throw new Error('trap'); }, set() { throw new Error('trap'); } });
  new Other(proxy);
  assert.strictEqual(Other.read(proxy), 'xy');
}

{
  class Base { who() { return 'base'; } static who() { return 'static base'; } }
  class Derived extends Base {
    order = [];
    #one() { return super.who() + 1; }
    #two() { return eval('super.who()') + 2; }
    *#gen() { yield this.#one(); yield this.#two(); }
    async #later() { return this.#one(); }
    async *#stream() { yield 1; }
    static #s1() { return super.who(); }
    static #s2() { return this.#s1() + '!'; }
    constructor() { const init = () => super(); init(); this.order.push('body'); }
    all() { return [...this.#gen()]; }
    later() { return this.#later(); }
    stream() { return this.#stream(); }
    static s() { return Derived.#s2(); }
  }
  const d = new Derived();
  assert.deepStrictEqual(d.all(), ['base1', 'base2']);
  assert.strictEqual(Derived.s(), 'static base!');
  assert.strictEqual(typeof d.stream()[Symbol.asyncIterator], 'function');
  d.later().then(v => assert.strictEqual(v, 'base1'));
  class Leaf extends Derived { #own() { return 'leaf'; } #more() { return this.#own(); } leaf() { return this.#more() + this.all().length; } }
  assert.strictEqual(new Leaf().leaf(), 'leaf2');
}

{
  const makers = [];
  for (let i = 0; i < 200; i++) {
    makers.push(class {
      #x() { return i; }
      #y() { return -i; }
      get #z() { return i * 2; }
      sum() { return this.#x() + this.#y() + this.#z; }
      fx() { return this.#x; }
      static has(o) { return #x in o; }
    });
  }
  for (let i = 0; i < 200; i++) {
    const a = new makers[i](), b = new makers[i]();
    assert.strictEqual(a.sum(), i * 2);
    assert.strictEqual(a.fx(), b.fx());
    if (i) { assert.notStrictEqual(a.fx(), new makers[i - 1]().fx()); assert.ok(!makers[i - 1].has(a)); }
  }
}

{
  let src = 'class Wide {';
  for (let i = 0; i < 300; i++) src += ` #m${i}() { return ${i}; }`;
  src += ' total() { let t = 0;';
  for (let i = 0; i < 300; i++) src += ` t += this.#m${i}();`;
  src += ' return t; } static has(o) { return #m0 in o && #m299 in o; } } return Wide;';
  const Wide = new Function(src)();
  const w = new Wide();
  assert.strictEqual(w.total(), 44850);
  assert.ok(Wide.has(w));
  assert.strictEqual(new Wide().total(), 44850);
}

{
  class One { #only() { return 1; } v() { return this.#only(); } }
  class Getter { get #only() { return 2; } v() { return this.#only; } }
  class Setter { set #only(v) { this.w = v; } v() { this.#only = 3; return this.w; } }
  class Accessor { get #p() { return this.w; } set #p(v) { this.w = v; } v() { this.#p = 4; return this.#p; } }
  class Nested { #a() { return class { #a() { return 'inner'; } #b() { return this.#a(); } v() { return this.#b(); } }; } #b() { return new (this.#a())().v(); } v() { return this.#b(); } }
  assert.deepStrictEqual([new One().v(), new Getter().v(), new Setter().v(), new Accessor().v(), new Nested().v()], [1, 2, 3, 4, 'inner']);
}

{
  class Hot { #a() { return 1; } #b() { return 2; } #c() { return 3; } n = 0; sum() { return this.#a() + this.#b() + this.#c(); } }
  let total = 0;
  for (let i = 0; i < 100000; i++) total += new Hot().sum();
  assert.strictEqual(total, 600000);
}

console.log('ok');
