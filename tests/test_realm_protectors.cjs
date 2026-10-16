const assert = require('node:assert');
const realm = require('ant:internal/realm');

function fresh() {
  const r = realm.create();
  return { r, g: r.global, run: (src) => realm.evaluate(r, src) };
}

const tick = () => new Promise((resolve) => setTimeout(resolve, 0));

function setThen(o, f) { o.then = f; }
for (let i = 0; i < 20000; i++) setThen(Promise.resolve(i), null);

async function main() {
  {
    const { g, run } = fresh();
    const seen = [];
    g.bp = run('Promise.resolve(1)');
    setThen(g.bp, function (resolve) { seen.push('own then'); resolve(2); });
    run('new Promise((resolve) => resolve(bp))');
    await tick();
    assert.deepStrictEqual(seen, ['own then']);
  }

  {
    const { g, run } = fresh();
    g.Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };
    assert.deepStrictEqual(run('[...[1, 2]]'), ['patched']);
  }

  {
    const { run } = fresh();
    const iterProto = run('Object.getPrototypeOf([][Symbol.iterator]())');
    const next = iterProto.next;
    iterProto.next = function () { const step = next.call(this); if (!step.done) step.value *= 10; return step; };
    assert.deepStrictEqual(run('[...[1, 2]]'), [10, 20]);
  }

  {
    const { g, run } = fresh();
    g.arr = run('[1, 2]');
    g.arr[Symbol.iterator] = function* () { yield 'own'; };
    assert.deepStrictEqual(run('[...arr]'), ['own']);
  }

  {
    const { g, run } = fresh();
    const seen = [];
    Object.defineProperty(g.Array, Symbol.species, { get() { seen.push('species'); return undefined; } });
    run('[1].map((x) => x)');
    assert.deepStrictEqual(seen, ['species']);
  }

  {
    const { g, run } = fresh();
    g.arr = run('[1, 2]');
    const seen = [];
    g.arr.constructor = { get [Symbol.species]() { seen.push('own species'); return undefined; } };
    run('arr.map((x) => x)');
    assert.deepStrictEqual(seen, ['own species']);
  }

  {
    const { g, run } = fresh();
    const seen = [];
    const then = g.Promise.prototype.then;
    g.Promise.prototype.then = function (...a) { seen.push('then'); return then.apply(this, a); };
    run('new Promise((resolve) => resolve(Promise.resolve(1)))');
    await tick();
    assert.ok(seen.includes('then'));
  }

  {
    const { g, run } = fresh();
    const seen = [];
    Object.defineProperty(g.Promise.prototype, 'constructor', {
      configurable: true,
      get() { seen.push('constructor'); return g.Promise; },
    });
    run('(async () => { await Promise.resolve(1); })()');
    await tick();
    assert.ok(seen.includes('constructor'));
  }

  {
    const { g, run } = fresh();
    g.Promise.resolve = () => 'patched resolve';
    assert.strictEqual(run('Promise.resolve(1)'), 'patched resolve');
  }
  {
    const { g, run } = fresh();
    g.Promise = { resolve: () => 'patched global' };
    assert.strictEqual(run('Promise.resolve(1)'), 'patched global');
  }

  {
    const { g, run } = fresh();
    g.RegExp.prototype.exec = () => null;
    assert.strictEqual(run('"aXb".replace(/X/, "-")'), 'aXb');
  }

  {
    const replaceX = new Function("return 'aXb'.replace(/X/, '-')");
    const exec = RegExp.prototype.exec;
    RegExp.prototype.exec = () => null;
    fresh();
    assert.strictEqual(replaceX(), 'aXb');
    RegExp.prototype.exec = exec;
  }

  {
    const { run } = fresh();
    /(b)(c)/.exec('abcd');
    run('/(x)(y)(z)/.exec("__xyz")');
    assert.deepStrictEqual([RegExp.$1, RegExp.$2, RegExp.lastMatch], ['b', 'c', 'bc']);
    assert.deepStrictEqual(run('[RegExp.$1, RegExp.$3, RegExp.lastMatch]'), ['x', 'z', 'xyz']);
  }

  {
    const { run } = fresh();
    assert.deepStrictEqual(run('[...[1, 2]]'), [1, 2]);
    assert.strictEqual(run('"aXb".replace(/X/, "-")'), 'a-b');
    assert.ok(!(run('[1].map((x) => x)') instanceof Array));
  }

  console.log('realm protectors: ok');
}

main();
