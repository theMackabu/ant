const assert = require('node:assert');

function child() {
  const out = [];
  const hot = (fn, rounds = 300) => { for (let r = 0; r < rounds; r++) fn(); };

  class Counter {
    #count = 0;
    #step;
    static #made = 0;
    constructor(step) { this.#step = step; Counter.#made++; }
    #bump() { this.#count += this.#step; return this.#count; }
    run(n) { let s = 0; for (let i = 0; i < n; i++) s += this.#bump(); return s; }
    get count() { return this.#count; }
    static made() { return Counter.#made; }
  }
  const counter = new Counter(2);
  hot(() => counter.run(100));
  out.push('counter ' + counter.run(10) + ' ' + counter.count + ' ' + Counter.made());

  class Wide {
    #a = 1; #b = 2; #c = 3; #d = 4; #e = 5; #f = 6; #g = 7; #h = 8; #i = 9; #j = 10; #k = 11; #l = 12;
    sum() { return this.#a + this.#b + this.#c + this.#d + this.#e + this.#f + this.#g + this.#h + this.#i + this.#j + this.#k + this.#l; }
    bump() { this.#a++; this.#f++; this.#l++; }
  }
  const wide = new Wide();
  let wideTotal = 0;
  hot(() => { wide.bump(); wideTotal += wide.sum(); });
  out.push('wide ' + wideTotal + ' ' + wide.sum());

  class Accessors {
    #raw = 1;
    get #doubled() { return this.#raw * 2; }
    set #doubled(v) { this.#raw = v / 2; }
    churn(n) { let s = 0; for (let i = 0; i < n; i++) { this.#doubled = i * 2; s += this.#doubled; } return s; }
    #method() { return this.#raw; }
    tryWriteMethod() { try { this.#method = 1; return 'wrote'; } catch (e) { return e.constructor.name; } }
  }
  const accessors = new Accessors();
  hot(() => accessors.churn(50));
  out.push('accessors ' + accessors.churn(10) + ' ' + accessors.tryWriteMethod());

  class Brand {
    #tag = 'b';
    static is(o) { return #tag in o; }
    static peek(o) { return o?.#tag; }
    static read(o) { return o.#tag; }
  }
  const branded = new Brand(), plain = {};
  let seen = 0;
  hot(() => { if (Brand.is(branded)) seen++; if (!Brand.is(plain)) seen++; if (Brand.peek(null) === undefined) seen++; if (Brand.peek(branded) === 'b') seen++; });
  const errors = [];
  for (const thunk of [() => Brand.read(plain), () => Brand.is(1), () => Brand.peek(plain), () => Brand.read(null)]) {
    try { thunk(); errors.push('none'); } catch (e) { errors.push(e.constructor.name); }
  }
  out.push('brand ' + seen + ' ' + errors.join(','));

  function makeClass(k) {
    return class { #v = k; get() { return this.#v; } set(v) { this.#v = v; } };
  }
  const instances = [0, 1, 2, 3].map(k => new (makeClass(k))());
  function readAll() { let s = 0; for (const o of instances) { o.set(o.get() + 1); s += o.get(); } return s; }
  hot(readAll);
  let crossError = '';
  try { instances[0].get.call(instances[1]); } catch (e) { crossError = e.constructor.name; }
  out.push('classes ' + readAll() + ' ' + crossError);

  class Box { #value; constructor(v) { this.#value = v; } swap(other) { const t = this.#value; this.#value = other.#value; other.#value = t; return this.#value; } }
  const left = new Box({ n: 1 }), right = new Box({ n: 2 });
  let swaps = 0;
  hot(() => { swaps += left.swap(right).n; });
  out.push('swap ' + swaps);

  class Grid {
    #cells = new Map();
    #w = 4;
    constructor() { for (let y = 0; y < 3; y++) for (let x = 0; x < this.#w; x++) this.#cells.set(`${x}-${y}`, x + y); }
    #at(x, y) { return this.#cells.get(`${x}-${y}`); }
    #has(x, y) { return this.#cells.has(`${x}-${y}`); }
    total() { let s = 0; for (let y = 0; y < 4; y++) for (let x = 0; x < this.#w; x++) s += this.#has(x, y) ? this.#at(x, y) : -1; return s; }
  }
  const grid = new Grid();
  hot(() => grid.total());
  out.push('grid ' + grid.total());

  console.log(out.join('\n'));
}

if (process.argv[2] === 'child') {
  child();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 180000,
    maxBuffer: 256 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  for (const [name, debug] of [['default', 'dump/vm:op-warn'], ['force', 'opt:force,verify']]) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    if (name === 'default' && /jit: opt compiled/.test(child.stderr)) {
      for (const fn of ['run', '#bump', 'sum', 'churn', 'is', 'peek', '#at', 'total', 'readAll'])
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${fn} must reach the opt tier`);
      assert.doesNotMatch(child.stderr, /opt rejected .*op=(GET_PRIVATE|PUT_PRIVATE|DEF_PRIVATE|GET_PRIVATE_OPT|HAS_PRIVATE|CALL_MAP_TEMPLATE|TAIL_MAP_TEMPLATE)\b/);
    }
  }
  const graphs = run({ ANT_DEBUG: 'dump/opt:graph' });
  assert.strictEqual(graphs.status, 0, graphs.stderr.slice(-4000));
  if (/=== optimized/.test(graphs.stdout + graphs.stderr))
    for (const node of ['LoadPrivate', 'StorePrivate'])
      assert.match(graphs.stdout + graphs.stderr, new RegExp(node), `${node} must appear in an optimized graph`);
  console.log('ok');
}
