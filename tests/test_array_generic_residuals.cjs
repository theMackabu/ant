const assert = require('node:assert');

const P = Array.prototype;
const show = v => { try { return JSON.stringify(v, (k, x) => x === undefined ? '__u' : typeof x === 'number' && !Number.isFinite(x) ? String(x) : x === 0 && 1 / x < 0 ? '-0' : x) } catch (e) { return 'unser' } };
const desc = o => Reflect.ownKeys(o).map(k => String(k) + '=' + show(o[k])).join(',');
const cases = [];
const add = (n, f) => cases.push([n, f]);
add('rev str len', () => desc(P.reverse.call({ length: '2', 0: 'a', 1: 'b' })));
add('rev obj len', () => desc(P.reverse.call({ length: { valueOf() { return 3 } }, 0: 'a', 2: 'c' })));
add('rev neg len', () => desc(P.reverse.call({ length: -1, 0: 'a' })));
add('rev frac len', () => desc(P.reverse.call({ length: 2.9, 0: 'a', 1: 'b', 2: 'c' })));
add('rev nan len', () => desc(P.reverse.call({ length: NaN, 0: 'a' })));
add('rev holes', () => desc(P.reverse.call({ length: 4, 0: 'a', 3: 'd' })));
add('rev holes2', () => desc(P.reverse.call({ length: 4, 1: 'b' })));
add('rev string', () => { try { return desc(P.reverse.call('ab')) } catch (e) { return e.name } });
add('rev arr holes', () => show([1, , 3, , 5].reverse()) + Object.keys([1, , 3, , 5].reverse()));
add('rev proto', () => { const p = { 1: 'p' }; const o = Object.create(p); o.length = 3; o[0] = 'a'; P.reverse.call(o); return desc(o) });
add('rev getter len', () => { let n = 0; const o = { get length() { n++; return 2 }, 0: 'a', 1: 'b' }; P.reverse.call(o); return n + desc(o).replace(/length=\d+,?/, '') });
add('rev big', () => { const o = { length: 2 ** 53 - 1, 0: 'a' }; let n = 0; const p = new Proxy(o, { has(t, k) { if (++n > 6) throw new Error('stop'); return k in t } }); try { P.reverse.call(p) } catch (e) { return e.message + n } });
for (const len of ['3', 3.7, -2, NaN, Infinity, -Infinity, 2 ** 32, 2 ** 32 + 2, 2 ** 53, 2 ** 53 - 1, 2 ** 53 + 2, { valueOf() { return 2 } }, null, undefined, true, '0x3', [2]]) {
  const mk = () => ({ length: len, 0: 'a', 1: 'b', 2: 'c', 4294967294: 'x', 4294967295: 'y', 4294967296: 'z', 9007199254740990: 'last', 9007199254740991: 'over' });
  const L = show(len);
  add('indexOf ' + L, () => [P.indexOf.call(mk(), 'b'), P.indexOf.call(mk(), 'c', -1), P.indexOf.call(mk(), 'y', 4294967290), P.indexOf.call(mk(), 'last', 9007199254740985), P.indexOf.call(mk(), 'last', -3)]);
  add('lastIndexOf ' + L, () => [P.lastIndexOf.call(mk(), 'b', 5), P.lastIndexOf.call(mk(), 'last', -1), P.lastIndexOf.call(mk(), 'a', 2), P.lastIndexOf.call(mk(), 'y', 4294967299), P.lastIndexOf.call(mk(), 'x', -(2 ** 53 - 4294967300))]);
  add('includes ' + L, () => [P.includes.call(mk(), 'b'), P.includes.call(mk(), undefined, 1) && 0, P.includes.call(mk(), 'y', 4294967290), P.includes.call(mk(), 'last', -3), P.includes.call(mk(), 'a', 1)]);
  add('fill ' + L, () => { const o = mk(); P.fill.call(o, 'F', -2, len > 100 ? -1 : undefined); return desc(o) });
  add('fill2 ' + L, () => { const o = mk(); P.fill.call(o, 'F', len > 100 ? 4294967294 : 0, len > 100 ? 4294967297 : 2); return desc(o) });
  add('copyWithin ' + L, () => { const o = mk(); P.copyWithin.call(o, 0, 1, 3); return desc(o) });
  add('copyWithin2 ' + L, () => { const o = mk(); P.copyWithin.call(o, len > 100 ? 4294967297 : 1, len > 100 ? 4294967294 : 0, len > 100 ? 4294967296 : 2); return desc(o) });
  add('copyWithin3 ' + L, () => { const o = mk(); P.copyWithin.call(o, -3, -2); return desc(o) });
  add('splice ' + L, () => { const o = mk(); const r = P.splice.call(o, len > 100 ? -2 : 1, 1, 'S1', 'S2'); return show(r) + '|' + desc(o) });
  add('splice del ' + L, () => { const o = mk(); const r = P.splice.call(o, len > 100 ? -3 : 0, 2); return show(r) + '|' + desc(o) });
  add('splice over ' + L, () => { const o = mk(); try { const r = P.splice.call(o, len > 100 ? -1 : 0, 0, 'S1', 'S2'); return show(r) + '|' + desc(o) } catch (e) { return e.name + '|' + desc(o) } });
  add('splice none ' + L, () => { const o = mk(); const r = P.splice.call(o); return show(r) + '|' + desc(o) });
  add('push ' + L, () => { const o = mk(); try { return P.push.call(o, 'P') + '|' + desc(o) } catch (e) { return e.name + '|' + desc(o) } });
  add('pop ' + L, () => { const o = mk(); try { return show(P.pop.call(o)) + '|' + desc(o) } catch (e) { return e.name + '|' + desc(o) } });
  add('at ' + L, () => [P.at.call(mk(), -1), P.at.call(mk(), 1), P.at.call(mk(), -2)]);
  add('with ' + L, () => { try { return show(P.with.call(mk(), 1, 'W').length) } catch (e) { return e.name } });
  add('join ' + L, () => { if (len > 100 || len === Infinity) return 'skip'; return P.join.call(mk()) });
  add('unshift0 ' + L, () => { const o = mk(); try { return P.unshift.call(o) + '|' + desc(o) } catch (e) { return e.name } });
  add('findLast ' + L, () => { let n = 0; return show(P.findLast.call(mk(), (v, i) => { return ++n > 3 || v === 'last' })) + n });
  add('findLastIndex ' + L, () => { let n = 0; return show(P.findLastIndex.call(mk(), (v, i) => ++n > 3)) });
}
const sortIn = [
  () => [3, , 1, undefined, 2], () => [, , ,], () => [undefined, , 1], () => [, 'b', , 'a', undefined, , 'c'], () => { const a = [3, 1, 2]; a.length = 6; return a },
  () => { const a = []; a[5] = 1; a[2] = 3; return a }, () => { const a = [2, 1]; a[10] = 0; return a }, () => ({ length: 5, 0: 'c', 2: 'a', 4: undefined }),
  () => ({ length: 4, 3: 'x', 1: undefined }), () => { const o = Object.create({ 1: 'proto' }); o.length = 4; o[0] = 'z'; o[3] = 'a'; return o },
  () => { const a = [3, , 1]; Object.setPrototypeOf(a, Object.assign(Object.create(Array.prototype), { 1: 'p' })); return a }, () => ({ length: '3', 0: 'b', 2: 'a' }),
  () => [1, , NaN, , -0, 0, undefined, null], () => { const a = new Array(40); a[39] = 1; a[0] = 2; a[20] = undefined; return a },
  () => { const a = []; for (let i = 0; i < 30; i++) if (i % 3) a[i] = 30 - i; return a },
];
sortIn.forEach((mk, i) => {
  add('sort ' + i, () => { const o = mk(); P.sort.call(o); return desc(o) + '|' + Object.keys(o).length });
  add('sort cmp ' + i, () => { const o = mk(); let n = 0; P.sort.call(o, (a, b) => { n++; return a < b ? 1 : a > b ? -1 : 0 }); return desc(o) + (n > 0) });
  add('toSorted ' + i, () => { const o = mk(); const r = P.toSorted.call(o); return desc(r) + '|' + desc(o) });
  add('sort calls ' + i, () => { const o = mk(); const seen = []; P.sort.call(o, (a, b) => { seen.push(a === undefined || b === undefined); return 0 }); return seen.some(Boolean) });
});
add('sort accessor', () => { const log = []; const o = { length: 3, get 0() { log.push('g0'); return 'b' }, set 0(v) { log.push('s0' + v) }, 2: 'a' }; P.sort.call(o); return log.join() + '|' + Object.keys(o) });
add('sort frozen hole', () => { const a = Object.freeze([2, , 1]); try { a.sort(); return 'ok' } catch (e) { return e.name } });
add('sort sealed hole', () => { const a = Object.seal([2, , 1]); try { a.sort(); return show(a) } catch (e) { return e.name + show(a) } });
add('sort nonwritable', () => { const a = [2, , 1]; Object.defineProperty(a, 0, { writable: false }); try { a.sort(); return show(a) } catch (e) { return e.name + show(a) } });
add('sort nonconfig del', () => { const o = { length: 3, 0: 'b' }; Object.defineProperty(o, 2, { value: 'a', configurable: false, writable: true, enumerable: true }); try { P.sort.call(o); return desc(o) } catch (e) { return e.name + desc(o) } });

function logged(t) { const log = []; const p = new Proxy(t, {
  has(t, k) { log.push('has ' + String(k)); return Reflect.has(t, k) }, get(t, k, r) { log.push('get ' + String(k)); return Reflect.get(t, k, r) },
  set(t, k, v, r) { log.push('set ' + String(k) + '=' + String(v)); return Reflect.set(t, k, v, r) }, deleteProperty(t, k) { log.push('del ' + String(k)); return Reflect.deleteProperty(t, k) },
  defineProperty(t, k, d) { log.push('def ' + String(k)); return Reflect.defineProperty(t, k, d) }, getOwnPropertyDescriptor(t, k) { log.push('gopd ' + String(k)); return Reflect.getOwnPropertyDescriptor(t, k) } }); return [p, log]; }
const proxyCases = {
  'reverse arr': () => { const [p, l] = logged([1, 2, 3, 4]); P.reverse.call(p); return l },
  'reverse holes': () => { const [p, l] = logged([1, , , 4, , 6]); P.reverse.call(p); return l },
  'reverse obj': () => { const [p, l] = logged({ length: '3', 0: 'a', 2: 'c' }); P.reverse.call(p); return l },
  'reverse len neg': () => { const [p, l] = logged({ length: -5, 0: 'a' }); P.reverse.call(p); return l },
  'copyWithin fwd': () => { const [p, l] = logged([1, 2, 3, 4, 5]); P.copyWithin.call(p, 0, 3); return l },
  'copyWithin back': () => { const [p, l] = logged([1, 2, 3, 4, 5]); P.copyWithin.call(p, 1, 0, 3); return l },
  'copyWithin nonoverlap up': () => { const [p, l] = logged([1, 2, 3, 4, 5, 6]); P.copyWithin.call(p, 4, 0, 2); return l },
  'copyWithin holes': () => { const [p, l] = logged([1, , 3, , 5]); P.copyWithin.call(p, 0, 1, 4); return l },
  'copyWithin big': () => { const [p, l] = logged({ length: 2 ** 53 - 1, 4294967296: 'a' }); P.copyWithin.call(p, 2 ** 53 - 3, 4294967295, 4294967298); return l },
  'splice mid': () => { const [p, l] = logged([1, 2, 3, 4, 5]); P.splice.call(p, 1, 2, 'x'); return l },
  'splice grow': () => { const [p, l] = logged([1, 2, 3]); P.splice.call(p, 1, 0, 'x', 'y'); return l },
  'splice big': () => { const [p, l] = logged({ length: 2 ** 32 + 1, 4294967295: 'q' }); P.splice.call(p, -2, 1); return l },
  'lastIndexOf': () => { const [p, l] = logged([1, , 3]); P.lastIndexOf.call(p, 9); return l.map(x => x.replace(/^get/, 'G')).sort() },
  'lastIndexOf big': () => { const [p, l] = logged({ length: 2 ** 40, 1099511627775: 'z', 1099511627773: 'y' }); return [P.lastIndexOf.call(p, 'y')] },
  'sort': () => { const [p, l] = logged([3, , 1]); P.sort.call(p); return l.filter(x => !/^(has|get) /.test(x)) },
  'sort obj': () => { const [p, l] = logged({ length: 4, 0: 'b', 3: 'a' }); P.sort.call(p); return l.filter(x => !/^(has|get) /.test(x)) },
  'fill': () => { const [p, l] = logged({ length: 3 }); P.fill.call(p, 7, 1); return l },
  'pop': () => { const [p, l] = logged([1, 2]); P.pop.call(p); return l },
  'len string': () => { const [p, l] = logged({ length: '2', 0: 'a', 1: 'b' }); return [P.indexOf.call(p, 'b'), P.join.call(p), P.slice.call(p).length, P.map.call(p, x => x).length, P.includes.call(p, 'b'), P.at.call(p, -1)] },
  'len obj': () => { const [p, l] = logged({ length: { valueOf() { return 2 } }, 0: 'a', 1: 'b' }); return [P.indexOf.call(p, 'b'), P.join.call(p), P.lastIndexOf.call(p, 'a')] },
  'len neg': () => { const [p, l] = logged({ length: -1, 0: 'a' }); return [P.indexOf.call(p, 'a'), P.join.call(p), P.push.call(p, 1)] },
  'len inf': () => { const [p, l] = logged({ length: Infinity, 0: 'a' }); return [P.indexOf.call(p, 'a'), P.at.call(p, 0)] },
  'len bigint': () => { const [p, l] = logged({ length: 2n, 0: 'a' }); try { return [P.indexOf.call(p, 'a')] } catch (e) { return [e.name] } },
  'len symbol': () => { const [p, l] = logged({ length: Symbol(), 0: 'a' }); try { return [P.indexOf.call(p, 'a')] } catch (e) { return [e.name] } },
};

const expectedArray = {
  "rev str len": "0=\"b\",1=\"a\",length=\"2\"",
  "rev obj len": "0=\"c\",2=\"a\",length={}",
  "rev holes2": "2=\"b\",length=4",
  "rev getter len": "10=\"b\",1=\"a\",",
  "rev big": "stop7",
  "indexOf \"3\"": "[1,2,-1,-1,-1]",
  "copyWithin2 \"3\"": "0=\"a\",1=\"a\",2=\"b\",4294967294=\"x\",length=\"3\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "push \"3\"": "4|0=\"a\",1=\"b\",2=\"c\",3=\"P\",4294967294=\"x\",length=4,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "findLast \"3\"": "\"__u\"3",
  "fill2 3.7": "0=\"F\",1=\"F\",2=\"c\",4294967294=\"x\",length=3.7,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice over 3.7": "[]|0=\"S1\",1=\"S2\",2=\"a\",3=\"b\",4=\"c\",4294967294=\"x\",length=5,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "join 3.7": "a,b,c",
  "includes -2": "[false,false,false,false,false]",
  "splice -2": "[]|0=\"S1\",1=\"S2\",2=\"c\",4294967294=\"x\",length=2,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "at -2": "[\"__u\",\"__u\",\"__u\"]",
  "indexOf \"NaN\"": "[-1,-1,-1,-1,-1]",
  "copyWithin2 \"NaN\"": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=\"NaN\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "push \"NaN\"": "1|0=\"P\",1=\"b\",2=\"c\",4294967294=\"x\",length=1,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "findLast \"NaN\"": "\"__u\"0",
  "indexOf \"Infinity\"": "[1,-1,4294967295,9007199254740990,9007199254740990]",
  "lastIndexOf \"Infinity\"": "[1,9007199254740990,0,4294967295,4294967294]",
  "copyWithin \"Infinity\"": "0=\"b\",1=\"c\",2=\"c\",4294967294=\"x\",length=\"Infinity\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin2 \"Infinity\"": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=\"Infinity\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"x\",4294967298=\"y\"",
  "copyWithin3 \"Infinity\"": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=\"Infinity\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",9007199254740989=\"last\"",
  "splice del \"Infinity\"": "[\"__u\",\"__u\"]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740989,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\",9007199254740988=\"last\"",
  "splice over \"Infinity\"": "TypeError|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=\"Infinity\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice none \"Infinity\"": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "pop \"Infinity\"": "\"last\"|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740990,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\"",
  "at \"Infinity\"": "[\"last\",\"b\",\"__u\"]",
  "unshift0 \"Infinity\"": "9007199254740991|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "findLast \"Infinity\"": "\"last\"1",
  "fill \"-Infinity\"": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=\"-Infinity\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice del \"-Infinity\"": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=0,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "with \"-Infinity\"": "RangeError",
  "fill 4294967296": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"F\",length=4294967296,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin 4294967296": "0=\"b\",1=\"c\",2=\"c\",4294967294=\"x\",length=4294967296,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin3 4294967296": "0=\"a\",1=\"b\",2=\"c\",4294967293=\"x\",4294967294=\"y\",length=4294967296,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice 4294967296": "[\"x\"]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"S1\",length=4294967297,4294967295=\"S2\",4294967296=\"y\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice del 4294967296": "[\"__u\",\"x\"]|0=\"a\",1=\"b\",2=\"c\",4294967293=\"y\",length=4294967294,4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice over 4294967296": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967298,4294967295=\"S1\",4294967296=\"S2\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"y\"",
  "splice none 4294967296": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967296,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "with 4294967296": "RangeError",
  "fill 4294967298": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967298,4294967295=\"y\",4294967296=\"F\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin 4294967298": "0=\"b\",1=\"c\",2=\"c\",4294967294=\"x\",length=4294967298,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin2 4294967298": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967298,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"x\"",
  "copyWithin3 4294967298": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967298,4294967295=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice 4294967298": "[\"z\"]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967299,4294967295=\"y\",4294967296=\"S1\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"S2\"",
  "splice del 4294967298": "[\"y\",\"z\"]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967296,9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice over 4294967298": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967300,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"S1\",4294967298=\"S2\"",
  "splice none 4294967298": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=4294967298,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "with 4294967298": "RangeError",
  "indexOf 9007199254740992": "[1,-1,4294967295,9007199254740990,9007199254740990]",
  "lastIndexOf 9007199254740992": "[1,9007199254740990,0,4294967295,4294967294]",
  "copyWithin 9007199254740992": "0=\"b\",1=\"c\",2=\"c\",4294967294=\"x\",length=9007199254740992,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin2 9007199254740992": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740992,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"x\",4294967298=\"y\"",
  "copyWithin3 9007199254740992": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740992,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",9007199254740989=\"last\"",
  "splice del 9007199254740992": "[\"__u\",\"__u\"]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740989,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\",9007199254740988=\"last\"",
  "splice over 9007199254740992": "TypeError|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740992,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice none 9007199254740992": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "pop 9007199254740992": "\"last\"|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740990,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\"",
  "at 9007199254740992": "[\"last\",\"b\",\"__u\"]",
  "findLast 9007199254740992": "\"last\"1",
  "indexOf 9007199254740991": "[1,-1,4294967295,9007199254740990,9007199254740990]",
  "lastIndexOf 9007199254740991": "[1,9007199254740990,0,4294967295,4294967294]",
  "copyWithin 9007199254740991": "0=\"b\",1=\"c\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin2 9007199254740991": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"x\",4294967298=\"y\"",
  "copyWithin3 9007199254740991": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",9007199254740989=\"last\"",
  "splice del 9007199254740991": "[\"__u\",\"__u\"]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740989,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\",9007199254740988=\"last\"",
  "splice over 9007199254740991": "TypeError|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice none 9007199254740991": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "pop 9007199254740991": "\"last\"|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740990,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\"",
  "at 9007199254740991": "[\"last\",\"b\",\"__u\"]",
  "findLast 9007199254740991": "\"last\"1",
  "findLastIndex 9007199254740991": "9007199254740987",
  "indexOf 9007199254740994": "[1,-1,4294967295,9007199254740990,9007199254740990]",
  "lastIndexOf 9007199254740994": "[1,9007199254740990,0,4294967295,4294967294]",
  "copyWithin 9007199254740994": "0=\"b\",1=\"c\",2=\"c\",4294967294=\"x\",length=9007199254740994,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "copyWithin2 9007199254740994": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740994,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",4294967297=\"x\",4294967298=\"y\"",
  "copyWithin3 9007199254740994": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740994,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\",9007199254740989=\"last\"",
  "splice del 9007199254740994": "[\"__u\",\"__u\"]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740989,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\",9007199254740988=\"last\"",
  "splice over 9007199254740994": "TypeError|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740994,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice none 9007199254740994": "[]|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740991,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "push 9007199254740994": "TypeError|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740994,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "pop 9007199254740994": "\"last\"|0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=9007199254740990,4294967295=\"y\",4294967296=\"z\",9007199254740991=\"over\"",
  "at 9007199254740994": "[\"last\",\"b\",\"__u\"]",
  "findLast 9007199254740994": "\"last\"1",
  "fill2 {}": "0=\"F\",1=\"F\",2=\"c\",4294967294=\"x\",length={},4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice over {}": "[]|0=\"S1\",1=\"S2\",2=\"a\",3=\"b\",4294967294=\"x\",length=4,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "join {}": "a,b",
  "includes null": "[false,false,false,false,false]",
  "splice null": "[]|0=\"S1\",1=\"S2\",2=\"c\",4294967294=\"x\",length=2,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "at null": "[\"__u\",\"__u\",\"__u\"]",
  "indexOf \"__u\"": "[-1,-1,-1,-1,-1]",
  "copyWithin2 \"__u\"": "0=\"a\",1=\"b\",2=\"c\",4294967294=\"x\",length=\"__u\",4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "push \"__u\"": "1|0=\"P\",1=\"b\",2=\"c\",4294967294=\"x\",length=1,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "findLast \"__u\"": "\"__u\"0",
  "fill2 true": "0=\"F\",1=\"b\",2=\"c\",4294967294=\"x\",length=true,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "splice over true": "[]|0=\"S1\",1=\"S2\",2=\"a\",4294967294=\"x\",length=3,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "join true": "a",
  "includes \"0x3\"": "[true,false,false,false,false]",
  "splice \"0x3\"": "[\"b\"]|0=\"a\",1=\"S1\",2=\"S2\",3=\"c\",4294967294=\"x\",length=4,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "at \"0x3\"": "[\"c\",\"b\",\"b\"]",
  "indexOf [2]": "[1,-1,-1,-1,-1]",
  "copyWithin2 [2]": "0=\"a\",1=\"a\",2=\"c\",4294967294=\"x\",length=[2],4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "push [2]": "3|0=\"a\",1=\"b\",2=\"P\",4294967294=\"x\",length=3,4294967295=\"y\",4294967296=\"z\",9007199254740990=\"last\",9007199254740991=\"over\"",
  "findLast [2]": "\"__u\"2",
  "sort 1": "length=3|0",
  "toSorted 2": "0=1,1=\"__u\",2=\"__u\",length=3|0=\"__u\",2=1,length=3",
  "sort 4": "0=1,1=2,2=3,length=6|3",
  "toSorted 5": "0=1,1=3,2=\"__u\",3=\"__u\",4=\"__u\",5=\"__u\",length=6|2=3,5=1,length=6",
  "sort 7": "0=\"a\",1=\"c\",2=\"__u\",length=5|4",
  "toSorted 8": "0=\"x\",1=\"__u\",2=\"__u\",3=\"__u\",length=4|1=\"__u\",3=\"x\",length=4",
  "sort 10": "0=1,1=3,2=\"p\",length=3|3",
  "toSorted 11": "0=\"a\",1=\"b\",2=\"__u\",length=3|0=\"b\",2=\"a\",length=\"3\"",
  "sort 13": "0=1,1=2,2=\"__u\",length=40|3",
  "toSorted 14": "0=1,1=10,2=11,3=13,4=14,5=16,6=17,7=19,8=2,9=20,10=22,11=23,12=25,13=26,14=28,15=29,16=4,17=5,18=7,19=8,20=\"__u\",21=\"__u\",22=\"__u\",23=\"__u\",24=\"__u\",25=\"__u\",26=\"__u\",27=\"__u\",28=\"__u\",29=\"__u\",length=30|1=29,2=28,4=26,5=25,7=23,8=22,10=20,11=19,13=17,14=16,16=14,17=13,19=11,20=10,22=8,23=7,25=5,26=4,28=2,29=1,length=30",
  "sort sealed hole": "TypeError[1,\"__u\",1]",
  "sort nonwritable": "TypeError[2,\"__u\",1]",
  "sort nonconfig del": "TypeError0=\"a\",1=\"b\",2=\"a\",length=3",
};

const expectedProxy = {
  "reverse arr": "get length | has 0 | get 0 | has 3 | get 3 | set 0=4 | gopd 0 | def 0 | set 3=1 | gopd 3 | def 3 | has 1 | get 1 | has 2 | get 2 | set 1=3 | gopd 1 | def 1 | set 2=2 | gopd 2 | def 2",
  "reverse holes": "get length | has 0 | get 0 | has 5 | get 5 | set 0=6 | gopd 0 | def 0 | set 5=1 | gopd 5 | def 5 | has 1 | has 4 | has 2 | has 3 | get 3 | set 2=4 | gopd 2 | def 2 | del 3",
  "reverse obj": "get length | has 0 | get 0 | has 2 | get 2 | set 0=c | gopd 0 | def 0 | set 2=a | gopd 2 | def 2",
  "copyWithin fwd": "get length | has 3 | get 3 | set 0=4 | gopd 0 | def 0 | has 4 | get 4 | set 1=5 | gopd 1 | def 1",
  "copyWithin back": "get length | has 2 | get 2 | set 3=3 | gopd 3 | def 3 | has 1 | get 1 | set 2=2 | gopd 2 | def 2 | has 0 | get 0 | set 1=1 | gopd 1 | def 1",
  "copyWithin nonoverlap up": "get length | has 0 | get 0 | set 4=1 | gopd 4 | def 4 | has 1 | get 1 | set 5=2 | gopd 5 | def 5",
  "copyWithin holes": "get length | has 1 | del 0 | has 2 | get 2 | set 1=3 | gopd 1 | def 1 | has 3 | del 2",
  "copyWithin big": "get length | has 4294967295 | del 9007199254740989 | has 4294967296 | get 4294967296 | set 9007199254740990=a | gopd 9007199254740990 | def 9007199254740990",
  "splice big": "get length | has 4294967295 | get 4294967295 | has 4294967296 | del 4294967295 | del 4294967296 | set length=4294967296 | gopd length | def length",
  "lastIndexOf": "G 0 | G 2 | G length | has 0 | has 1 | has 2",
  "lastIndexOf big": "1099511627773",
  "len string": "1 | a,b | 2 | 2 | true | b",
  "len obj": "1 | a,b | 0",
  "len bigint": "TypeError",
  "len symbol": "TypeError",
};

const failed = [];
for (const [name, run] of cases) {
  if (!(name in expectedArray)) continue;
  let got;
  try { got = run(); } catch (e) { got = 'THROW ' + e.name + ': ' + e.message; }
  if (typeof got !== 'string') got = show(got);
  if (got !== expectedArray[name]) failed.push(name + ': expected ' + expectedArray[name] + ', got ' + got);
}
for (const [name, run] of Object.entries(proxyCases)) {
  if (!(name in expectedProxy)) continue;
  let got;
  try { got = run().join(' | '); } catch (e) { got = 'THROW ' + e.name + ': ' + e.message; }
  if (got !== expectedProxy[name]) failed.push(name + ': expected ' + expectedProxy[name] + ', got ' + got);
}

assert.deepStrictEqual(failed, []);

{
  const big = { length: 2 ** 53 - 1 };
  const removed = Array.prototype.splice.call(big, -2, 1, 'a', 'b');
  assert.strictEqual(removed.length, 1);
  assert.strictEqual(big.length, 2 ** 53);
  assert.deepStrictEqual(Object.keys(big), ['length', '9007199254740989', '9007199254740990']);

  const traced = (source, limit) => {
    const log = [];
    let has = 0;
    const proxy = new Proxy(source, {
      has(t, k) { log.push('has ' + String(k)); if (++has > limit) throw new RangeError('stop'); return Reflect.has(t, k); },
      get(t, k, r) { if (k !== 'length') log.push('get ' + String(k)); return Reflect.get(t, k, r); },
      set(t, k, v, r) { log.push('set ' + String(k)); return Reflect.set(t, k, v, r); },
      deleteProperty(t, k) { log.push('del ' + String(k)); return Reflect.deleteProperty(t, k); },
    });
    return { proxy, log };
  };

  let run = traced({ length: 2 ** 32 + 2, [2 ** 32]: 'x', [2 ** 32 + 1]: 'y' }, 4);
  assert.throws(() => Array.prototype.unshift.call(run.proxy, 'q'), RangeError);
  assert.deepStrictEqual(run.log, [
    'has 4294967297', 'get 4294967297', 'set 4294967298', 'has 4294967296', 'get 4294967296', 'set 4294967297',
    'has 4294967295', 'del 4294967296', 'has 4294967294', 'del 4294967295', 'has 4294967293',
  ]);

  run = traced({ length: 2 ** 32 + 2, 0: 'a', [2 ** 32]: 'x', [2 ** 32 + 1]: 'y' }, 3);
  assert.throws(() => Array.prototype.shift.call(run.proxy), RangeError);
  assert.deepStrictEqual(run.log, ['get 0', 'has 1', 'del 0', 'has 2', 'del 1', 'has 3', 'del 2', 'has 4']);

  run = traced([1, , 3], 100);
  assert.strictEqual(run.proxy.unshift(0), 4);
  assert.deepStrictEqual(run.log, ['get unshift', 'has 2', 'get 2', 'set 3', 'has 1', 'del 2', 'has 0', 'get 0', 'set 1', 'set 0', 'set length']);

  const layout = (a) => { const out = []; for (let i = 0; i < a.length; i++) out.push(Object.hasOwn(a, i) ? a[i] : '-'); return out.join(',') + '|' + a.length; };
  for (let round = 0; round < 200; round++) {
    let a = [1, 2, 3, , 5]; a.length = 9; a.unshift('x', 'y');
    assert.strictEqual(layout(a), 'x,y,1,2,3,-,5,-,-,-,-|11');
    a = [1, , 3]; a.length = 6; a[5] = 'z'; a.unshift(0);
    assert.strictEqual(layout(a), '0,1,-,3,-,-,z|7');
    a = [1, , 3]; a.length = 6; a[20] = 'far'; a.unshift(0);
    assert.strictEqual(layout(a), '0,1,-,3,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,far|22');
    a = []; a.length = 4; a.unshift(7);
    assert.strictEqual(layout(a), '7,-,-,-,-|5');
    const plain = new Proxy([1, 2, 3], {});
    plain.unshift(0); assert.strictEqual(plain.shift(), 0); assert.strictEqual(plain.length, 3);
  }

  Array.prototype[1] = 'P';
  try {
    let a = [1, , 3]; a.length = 5; a.unshift(0);
    assert.strictEqual(layout(a), '0,1,P,3,-,-|6');
    a = [1, , 3]; a.unshift(0);
    assert.strictEqual(layout(a), '0,1,P,3|4');
    a = [1, , 3]; a.shift();
    assert.strictEqual(layout(a), 'P,3|2');
  } finally {
    delete Array.prototype[1];
  }
  assert.throws(() => Object.freeze([1, 2]).unshift(0), TypeError);
  assert.strictEqual(Array.prototype.unshift.call({ length: 2 ** 53 - 1 }), 2 ** 53 - 1);
  assert.throws(() => Array.prototype.unshift.call({ length: 2 ** 53 - 1 }, 1), TypeError);
}

{
  const A = Array.prototype;
  const huge = (n) => ({ length: n, 0: 'a', 1: 'b' });
  const spread = (n) => Object.assign(huge(n), { [Symbol.isConcatSpreadable]: true });
  for (const n of [2 ** 32, 2 ** 53 - 1, Infinity]) {
    for (const make of [huge, (m) => new Proxy(huge(m), {})]) {
      assert.throws(() => A.slice.call(make(n), 0), RangeError, `slice ${n}`);
      assert.deepStrictEqual(A.slice.call(make(n), 0, 2), ['a', 'b']);
      assert.throws(() => A.join.call(make(n)), TypeError, `join ${n}`);
      assert.throws(() => A.toLocaleString.call(make(n)), TypeError, `toLocaleString ${n}`);
      assert.throws(() => A.map.call(make(n), (x) => x), RangeError, `map ${n}`);
      assert.throws(() => Array.from(make(n)), RangeError, `from ${n}`);
      assert.throws(() => A.toReversed.call(make(n)), RangeError, `toReversed ${n}`);
      assert.throws(() => A.toSorted.call(make(n)), RangeError, `toSorted ${n}`);
      assert.throws(() => A.with.call(make(n), 0, 1), RangeError, `with ${n}`);
      assert.throws(() => A.toSpliced.call(make(n), 0, 0), RangeError, `toSpliced ${n}`);
      assert.throws(() => A.splice.call(make(n), 0), RangeError, `splice ${n}`);
      assert.strictEqual(A.includes.call(make(n), 'b'), true);
      assert.strictEqual(A.indexOf.call(make(n), 'b'), 1);
      assert.strictEqual(A.find.call(make(n), (x, i) => i === 3), undefined);
      assert.deepStrictEqual(A.entries.call(make(n)).next().value, [0, 'a']);
      const keys = A.keys.call(make(n)); keys.next();
      assert.strictEqual(keys.next().value, 1);
      const values = A.values.call(make(n)); values.next(); values.next();
      assert.deepStrictEqual(values.next(), { value: undefined, done: false });
    }
    assert.strictEqual([].concat(spread(n)).length, 0, `concat ${n}`);
    assert.throws(() => Uint8Array.from(huge(2 ** 53 - 1)), RangeError);
  }
  assert.throws(() => [1].concat(spread(2 ** 53 - 1)), TypeError);
  assert.throws(() => A.toSpliced.call(huge(2 ** 53 - 1), 0, 0, 1), TypeError);
  assert.throws(() => A.push.call(new Proxy(huge(2 ** 53 - 1), {}), 1), TypeError);
  assert.strictEqual(A.slice.call(huge(2 ** 32), 2 ** 32 - 3).length, 3);

  const traps = (target, log) => new Proxy(target, {
    get(t, k, r) { log.push('get:' + String(k)); return Reflect.get(t, k, r); },
    set(t, k, v, r) { log.push('set:' + String(k)); return Reflect.set(t, k, v, r); },
    has(t, k) { log.push('has:' + String(k)); return Reflect.has(t, k); },
    deleteProperty(t, k) { log.push('del:' + String(k)); return Reflect.deleteProperty(t, k); },
  });
  for (const lock of [Object.freeze, Object.seal]) {
    assert.throws(() => A.pop.call(traps(lock(['c', 'a', 'b']), [])), TypeError, 'pop through a proxy of a locked array');
    for (const op of [(p) => A.shift.call(p), (p) => A.unshift.call(p, 1), (p) => A.push.call(p, 1), (p) => A.splice.call(p, 0, 1)])
      assert.throws(() => op(new Proxy(new Proxy(lock([3, 1, 2]), {}), {})), TypeError);
  }
  for (const op of [(p) => A.reverse.call(p), (p) => A.copyWithin.call(p, 0, 1), (p) => A.fill.call(p, 0), (p) => A.sort.call(p)])
    assert.throws(() => op(new Proxy(new Proxy(Object.freeze([3, 1, 2]), {}), {})), TypeError);

  const nested = (a) => new Proxy(new Proxy(a, {}), { has(t, k) { return Reflect.has(t, k); } });
  let target = [3, 1, 2];
  assert.strictEqual(A.shift.call(nested(target)), 3);
  assert.deepStrictEqual(target, [1, 2]);
  target = [3, 1, 2];
  assert.strictEqual(A.unshift.call(nested(target), 9), 4);
  assert.deepStrictEqual(target, [9, 3, 1, 2]);
  target = [3, 1, 2];
  A.reverse.call(nested(target));
  assert.deepStrictEqual(target, [2, 1, 3]);
  target = [3, 1, 2];
  A.copyWithin.call(nested(target), 0, 1);
  assert.deepStrictEqual(target, [1, 2, 2]);

  let log = [];
  A.shift.call(traps(['c', 'a'], log));
  assert.deepStrictEqual(log.slice(0, 4), ['get:length', 'get:0', 'has:1', 'get:1']);
  log = [];
  Array.from(traps(['c', 'a'], log));
  assert.deepStrictEqual(log, ['get:Symbol(Symbol.iterator)', 'get:length', 'get:0', 'get:length', 'get:1', 'get:length']);
  log = [];
  A.flat.call(traps(['c'], log));
  assert.deepStrictEqual(log, ['get:length', 'get:constructor', 'has:0', 'get:0']);
  log = [];
  A.toReversed.call(traps(['c', 'a', 'b'], log));
  assert.deepStrictEqual(log, ['get:length', 'get:2', 'get:1', 'get:0']);
  assert.deepStrictEqual([[1, [2, [3]]]].flat('2'), [1, 2, [3]]);
  assert.deepStrictEqual([[1, [2]]].flat({ valueOf() { return 0; } }), [[1, [2]]]);
  assert.deepStrictEqual([1, [2, [3, [4]]]].flat(Infinity), [1, 2, 3, 4]);
  for (const like of [{ 0: 'c' }, { 0: 'c', length: -1 }]) {
    assert.strictEqual(A.shift.call(like), undefined);
    assert.strictEqual(like.length, 0);
  }
}

const outcome = (make) => {
  try {
    return { value: make() };
  } catch (error) {
    return { error: error.name };
  }
};

const worksOrRangeError = (label, make, check) => {
  const result = outcome(make);
  if (result.error) assert.strictEqual(result.error, 'RangeError', label);
  else if (check) assert.ok(check(result.value), label);
};

{
  let nested = [];
  for (let i = 0; i < 1000000; i++) nested = [nested];
  worksOrRangeError('deep flat', () => nested.flat(Infinity).length);
}

console.log('ok');
