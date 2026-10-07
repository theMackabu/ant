// Natives that hold values in malloc'd buffers while JS runs (getters, traps,
// comparators, callbacks) must keep them rooted, and the sections here allocate
// heavily so a collection runs mid-call. Each runs in a fresh process and must
// print what Node prints; delete semantics ride along since the proxy and
// copyWithin paths share them.
const { spawnSync } = require('node:child_process');

const sections = [
  [function als() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const { AsyncLocalStorage } = require('node:async_hooks');
    const als = new AsyncLocalStorage();
    const r = als.run({ s: 1 }, (a, b) => { churn(30000); return ok(a) && ok(b) && als.getStore().s === 1; }, fresh(1), fresh(2));
    console.log('als', r);
  }, "als true\n"],
  [function copywithin() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const target = Array.from({ length: 20 }, (_, i) => 0);
    const p = new Proxy(target, { get(t, k) { if (/^\d+$/.test(k) && +k < 10) { churn(5000); return fresh(+k); } return t[k]; }, set(t, k, v) { churn(5000); t[k] = v; return true; } });
    Array.prototype.copyWithin.call(p, 10, 0, 10);
    console.log('copyWithin', target.slice(10).every((v, i) => v && v.payload === 'v' + i));
  }, "copyWithin true\n"],
  [function execfile() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const util = require('node:util');
    const { execFile } = require('node:child_process');
    const p = util.promisify(execFile)('/bin/echo', ['hi']);
    churn(50000);
    p.then(r => { churn(20000); console.log('execFile', r.stdout.trim()); });
  }, "execFile hi\n"],
  [function generator() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    function* g(a, b) { yield 1; churn(20000); yield [a.payload, b.payload]; }
    const its = []; for (let i = 0; i < 20; i++) its.push(g(fresh(i), fresh(i + 100)));
    churn(50000);
    const out = its.map(it => { it.next(); return it.next().value.join(','); });
    console.log('generator', out.every((s, i) => s === 'v' + i + ',v' + (i + 100)));
  }, "generator true\n"],
  [function pipeline() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const { pipeline } = require('node:stream/promises');
    const { Readable, Writable } = require('node:stream');
    const chunks = [];
    pipeline(Readable.from(['a', 'b', 'c']), new Writable({ write(c, e, cb) { churn(20000); chunks.push(String(c)); cb(); } }))
      .then(() => console.log('pipeline', chunks.join('')), e => console.log('pipeline err', e.message));
  }, "pipeline abc\n"],
  [function promisify() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const util = require('node:util');
    function op(a, cb) { churn(30000); setTimeout(() => { churn(30000); cb(null, a.payload); }, 1); }
    util.promisify(op)(fresh(7)).then(v => console.log('promisify', v));
  }, "promisify v7\n"],
  [function regexcaps() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const re = new RegExp('(a)'.repeat(24));
    const out = 'a'.repeat(24).replace(re, (...m) => { churn(30000); return m.slice(1, 25).join(''); });
    console.log('regex', out === 'a'.repeat(24));
  }, "regex true\n"],
  [function strconcat() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const parts = Array.from({ length: 16 }, (_, i) => ({ toString() { churn(5000); return 'p' + i + '-' + 'x'.repeat(30); } }));
    const s = ''.concat(...parts);
    console.log('concat', s === parts.map((_, i) => 'p' + i + '-' + 'x'.repeat(30)).join(''));
  }, "concat true\n"],
  [function tafilter() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const ta = BigInt64Array.from({ length: 40 }, (_, i) => BigInt(i) * 98765432109876n);
    const r = ta.filter(v => { churn(1000); return true; });
    console.log('tafilter', r.every((v, i) => v === BigInt(i) * 98765432109876n));
  }, "tafilter true\n"],
  [function tafrom() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    function* gen() { for (let i = 0; i < 40; i++) { churn(3000); yield BigInt(i) * 12345678901234567n; } }
    const ta = BigInt64Array.from(gen(), x => { churn(1000); return x + 1n; });
    console.log('tafrom', ta.every((v, i) => v === BigInt(i) * 12345678901234567n + 1n));
  }, "tafrom true\n"],
  [function tasort() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const ta = BigInt64Array.from({ length: 40 }, (_, i) => BigInt(40 - i) * 98765432109876n);
    ta.sort((a, b) => { churn(1000); return a < b ? -1 : a > b ? 1 : 0; });
    console.log('tasort', ta.every((v, i) => v === BigInt(i + 1) * 98765432109876n));
  }, "tasort true\n"],
  [function timelog2() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    console.time('LABEL');
    const big = () => Array.from({ length: 3000 }, (_, i) => ({ ['k' + i]: 'v' + i }));
    const orig = process.stdout.write;
    let captured = '';
    process.stdout.write = function (s) { captured += s; return true; };
    console.timeLog('LABEL', big(), big(), big(), big(), big(), big());
    process.stdout.write = orig;
    console.log('timelog', captured.startsWith('LABEL: '));
  }, "timelog true\n"],
  [function timers2() {
    globalThis.churn = (n = 3000) => { const j = []; for (let k = 0; k < n; k++) j.push({ k, s: 'x' + k }); return j.length; };
    globalThis.fresh = i => ({ id: i, payload: 'v' + i });
    globalThis.ok = v => v && v.payload === 'v' + v.id;
    const res = { timeout: [], tick: [], immediate: [] };
    for (let i = 0; i < 20; i++) setTimeout((a, b) => { churn(5000); res.timeout.push(ok(a) && ok(b)); }, 1, fresh(i), fresh(i + 50));
    for (let i = 0; i < 20; i++) process.nextTick((a) => { churn(5000); res.tick.push(ok(a)); }, fresh(i));
    for (let i = 0; i < 20; i++) setImmediate((a) => { churn(5000); res.immediate.push(ok(a)); }, fresh(i));
    churn(100000);
    setTimeout(() => console.log(JSON.stringify(Object.fromEntries(Object.entries(res).map(([k, v]) => [k, v.filter(x => !x).length])))), 50);
  }, "{\"timeout\":0,\"tick\":0,\"immediate\":0}\n"],
  [function immediates() {
    const out = [];
    setImmediate((a, b, c) => out.push([a, b, c.y]), 1, 'x', { y: 2 });
    const h = setImmediate(() => out.push('cleared by object ran'));
    clearImmediate(h);
    setImmediate(function () { out.push(arguments.length); });
    setTimeout(() => console.log(JSON.stringify(out)), 20);
  }, "[[1,\"x\",2],0]\n"],
  [function delsem() {
    const t = f => { try { return f(); } catch (e) { return e.constructor.name; } };
    const out = [];
    (function () { const fr = Object.freeze({ a: 1 }); const se = Object.seal([1, 2]); const nc = Object.defineProperty({}, 'x', { value: 1 });
      out.push(t(() => delete fr.a), t(() => delete fr.missing), t(() => delete se[0]), t(() => delete se[5]), t(() => delete se.length), t(() => delete nc.x), t(() => delete [1].length), t(() => delete Math.PI)); })();
    (function () { 'use strict'; const fr = Object.freeze({ a: 1 }); const se = Object.seal([1, 2]); const nc = Object.defineProperty({}, 'x', { value: 1 });
      out.push(t(() => delete fr.a), t(() => delete fr.missing), t(() => delete se[0]), t(() => delete se[5]), t(() => delete se.length), t(() => delete nc.x), t(() => delete [1].length), t(() => delete Math.PI)); })();
    (function () { 'use strict'; out.push(t(() => Reflect.deleteProperty(new Proxy({}, { deleteProperty() { throw new RangeError('trap'); } }), 'q'))); })();
    const a = [1, 2, 3]; delete a[1]; out.push(1 in a, a.length);
    console.log(JSON.stringify(out));
  }, "[false,true,false,true,false,false,false,false,\"TypeError\",true,\"TypeError\",true,\"TypeError\",\"TypeError\",\"TypeError\",\"TypeError\",\"RangeError\",false,3]\n"],
  [function proxydel() {
    const a = [1, 2, 3, 4]; const p = new Proxy(a, {});
    const r1 = delete p[3];
    const o = { x: 1, 0: 'z' }; const po = new Proxy(o, {});
    const r2 = delete po.x, r3 = delete po[0];
    const h = [1, , 3, 4]; [].copyWithin.call(new Proxy(h, {}), 2, 0);
    console.log(JSON.stringify([r1, 3 in a, a.length, r2, 'x' in o, r3, 0 in o, 3 in h]));
  }, "[true,false,4,true,false,true,false,false]\n"],
  [function proxydel2() {
    'use strict';
    const t = f => { try { return f(); } catch (e) { return e.constructor.name; } };
    const frozen = Object.freeze({ a: 1 }); const pf = new Proxy(frozen, {});
    const inner = new Proxy({ b: 1 }, {}); const outer = new Proxy(inner, {});
    console.log(JSON.stringify([t(() => delete pf.a), t(() => Reflect.deleteProperty(pf, 'a')), t(() => (delete outer.b, 'b' in inner)), t(() => Reflect.deleteProperty(new Proxy([1, 2], {}), 'length'))]));
  }, "[\"TypeError\",false,false,false]\n"],
  [function refdel() {
    'use strict';
    const t = f => { try { return f(); } catch (e) { return e.constructor.name; } };
    const fr = Object.freeze({ a: 1 });
    console.log(JSON.stringify([t(() => Reflect.deleteProperty(fr, 'a')), t(() => Reflect.deleteProperty([1], 'length')), t(() => delete fr.a), t(() => delete new Proxy(fr, {}).a), t(() => delete new Proxy({ x: 1 }, { deleteProperty() { return false; } }).x)]));
  }, "[false,false,\"TypeError\",\"TypeError\",\"TypeError\"]\n"],
];

let failures = 0;
const env = { ...process.env, NO_COLOR: '1' };
delete env.FORCE_COLOR;
for (const [section, expected] of sections) {
  const child = spawnSync(process.execPath, ['-e', `(${section})()`], { encoding: 'utf8', timeout: 60000, env });
  if (child.status !== 0 || child.stdout !== expected) {
    failures++;
    console.log(`FAIL ${section.name} (status ${child.status})\n--- got\n${child.stdout}${child.stderr}\n--- expected\n${expected}`);
  }
}
if (failures) throw new Error(`${failures} sections differ from Node`);
console.log('PASS native buffers stay rooted and match Node');
