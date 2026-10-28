const assert = require('node:assert');

const WARM = 3000;

function child() {
  function A() { this.x = 1; this.y = 2; }
  A.prototype.who = function () { return 'A'; };
  function B() { this.x = 1; this.y = 2; }
  B.prototype.who = function () { return 'B'; };
  function who(o) { return o.who() + o.x; }
  const mixed = [new A(), new B(), new A(), new B()];
  let out = '';
  for (let i = 0; i < WARM; i++) out = who(mixed[i & 3]);
  assert.strictEqual(out, 'B1');
  console.log('same layout, different prototypes', who(mixed[0]), who(mixed[1]));

  const moved = new A();
  for (let i = 0; i < WARM; i++) who(moved);
  Object.setPrototypeOf(moved, B.prototype);
  assert.strictEqual(who(moved), 'B1');
  moved.__proto__ = { who() { return 'literal'; } };
  assert.strictEqual(who(moved), 'literal1');
  console.log('setPrototypeOf with properties', who(moved));

  const proto = { who() { return 'created'; } };
  function viaCreate() { const o = Object.create(proto); o.x = 3; return who(o); }
  for (let i = 0; i < WARM; i++) assert.strictEqual(viaCreate(), 'created3');
  const viaLiteral = () => who({ __proto__: proto, x: 4 });
  for (let i = 0; i < WARM; i++) assert.strictEqual(viaLiteral(), 'created4');
  console.log('Object.create and __proto__ literals', viaCreate(), viaLiteral());

  class Base { constructor() { this.x = 5; } who() { return 'Base'; } }
  class Derived extends Base { who() { return 'Derived'; } }
  const instances = () => [new Base(), new Derived()].map(who).join();
  for (let i = 0; i < WARM; i++) assert.strictEqual(instances(), 'Base5,Derived5');
  console.log('classes', instances());

  const arrays = [];
  for (let i = 0; i < 50; i++) arrays.push([i]);
  const special = [1, 2, 3];
  Object.setPrototypeOf(special, { first() { return 'custom'; } });
  function firstOf(a) { return typeof a.first === 'function' ? a.first() : a.length; }
  for (let i = 0; i < WARM; i++) firstOf(arrays[i % 50]);
  assert.strictEqual(firstOf(special), 'custom');
  console.log('arrays', firstOf(arrays[0]), firstOf(special));

  function argsLength() { return arguments.length + Object.getPrototypeOf(arguments).constructor.name; }
  for (let i = 0; i < WARM; i++) assert.strictEqual(argsLength(1, 2), '2Object');
  console.log('arguments', argsLength(1, 2, 3));

  const parsed = JSON.parse('[{"x":1,"y":2},{"x":3,"y":4}]');
  Object.setPrototypeOf(parsed[1], B.prototype);
  for (let i = 0; i < WARM; i++) assert.strictEqual(typeof parsed[0].who, 'undefined');
  assert.strictEqual(who(parsed[1]), 'B3');
  const again = JSON.parse('{"x":7,"y":8}');
  assert.strictEqual(Object.getPrototypeOf(again), Object.prototype);
  console.log('JSON', who(parsed[1]), again.x);

  const matched = /(a)(b)/.exec('xab');
  Array.prototype.matchTag = function () { return 'tagged'; };
  const tags = [];
  for (let i = 0; i < 100; i++) tags.push(/(a)/.exec('a' + i).matchTag());
  delete Array.prototype.matchTag;
  assert.strictEqual(tags[99], 'tagged');
  console.log('regexp results', matched.index, tags[0]);

  const source = new A();
  const assigned = Object.assign({}, source);
  const spread = { ...source };
  assert.strictEqual(Object.getPrototypeOf(assigned), Object.prototype);
  assert.strictEqual(Object.getPrototypeOf(spread), Object.prototype);
  assert.strictEqual(typeof assigned.who, 'undefined');
  const target = Object.create(B.prototype);
  Object.assign(target, source);
  assert.strictEqual(who(target), 'B1');
  console.log('copies', assigned.x, spread.y, who(target));

  const nullProto = Object.create(null);
  nullProto.x = 1;
  assert.strictEqual(Object.getPrototypeOf(nullProto), null);
  Object.setPrototypeOf(nullProto, A.prototype);
  assert.strictEqual(who(nullProto), 'A1');
  console.log('null prototype', who(nullProto));
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
    timeout: 120000, maxBuffer: 256 * 1024 * 1024,
  });
  const interp = run({ ANT_DEBUG: 'shape:verify' }, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);
  assert.match(interp.stdout, /null prototype A1\n$/);
  const modes = [
    ['default', 'shape:verify'],
    ['verify', 'shape:verify opt:verify'],
    ['force', 'shape:verify opt:force,verify'],
    ['off', 'shape:verify opt:off'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr.slice(-4000)}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from --jitless`);
    console.log(`PASS shape proto identity ${name}`);
  }
}
