// Array builtins take dense fast paths (direct buffer reads, a species protector
// for %Array%, an iteration protector for spread/for-of/Array.from). Each section
// runs in a fresh process so the protectors start valid, and must print what
// Node prints: holes, inherited elements, species, proxies and patched iterators.
const { spawnSync } = require('node:child_process');

const sections = [
  [function arrsem() {
    const out = [];
    const show = v => { try { return JSON.stringify(v, (k, x) => x === undefined ? 'U' : Number.isNaN(x) ? 'NaN' : Object.is(x, -0) ? '-0' : x); } catch (e) { return 'ERR ' + e.name; } };
    const own = a => Array.isArray(a) ? [a.length, Object.keys(a).join()] : a;
    function run(name, fn) { let r; try { r = fn(); } catch (e) { r = 'THROW ' + e.name; } out.push(name + ' ' + show(r)); }
    const o = { x: 1 };
    const fixtures = () => ({ plain: [1, 2, 3, 2, 1], holey: [1, , 3, , 5], mixed: [NaN, -0, 0, 'a', o, undefined, null, 2n], big: Array.from({ length: 40 }, (_, k) => k % 7) });
    for (const [fname, mk] of Object.entries({ plain: () => fixtures().plain, holey: () => fixtures().holey, mixed: () => fixtures().mixed, big: () => fixtures().big })) {
      run(fname + ' indexOf', () => { const a = mk(); return [a.indexOf(2), a.indexOf(3, 3), a.indexOf(NaN), a.indexOf(0), a.indexOf(-0), a.indexOf('a'), a.indexOf(o), a.indexOf(undefined), a.indexOf(null), a.indexOf(2n), a.indexOf(1, -2)]; });
      run(fname + ' lastIndexOf', () => { const a = mk(); return [a.lastIndexOf(2), a.lastIndexOf(1, 2), a.lastIndexOf(NaN), a.lastIndexOf(0), a.lastIndexOf(undefined), a.lastIndexOf(2n), a.lastIndexOf(1, -1)]; });
      run(fname + ' fill', () => own(mk().fill(9, 1, 3)));
      run(fname + ' fillAll', () => own(mk().fill(7)));
      run(fname + ' concat', () => own(mk().concat(mk(), 5, [6, , 8])));
      run(fname + ' toReversed', () => own(mk().toReversed()));
      run(fname + ' toSorted', () => own(mk().toSorted()));
      run(fname + ' toSpliced', () => own(mk().toSpliced(1, 2, 'x')));
      run(fname + ' with', () => own(mk().with(1, 'w')));
      run(fname + ' map', () => own(mk().map(x => typeof x)));
      run(fname + ' filter', () => own(mk().filter(x => x !== 1)));
      run(fname + ' forEach', () => { const r = []; mk().forEach((x, i) => r.push(i)); return r; });
      run(fname + ' reduce', () => mk().reduce((s, x, i) => s + ':' + i, ''));
      run(fname + ' some/every', () => { const a = mk(); return [a.some(x => x === 3), a.every(x => x !== 9)]; });
      run(fname + ' slice', () => own(mk().slice(1, 4)));
      run(fname + ' splice', () => { const a = mk(); const r = a.splice(1, 2); return [own(r), own(a)]; });
      run(fname + ' flat', () => own([mk(), [mk()]].flat()));
    }
    run('map grows', () => { const a = [1, 2, 3]; return own(a.map((x, i) => { if (i === 0) a.push(9); return x; })); });
    run('forEach shrinks', () => { const a = [1, 2, 3, 4]; const r = []; a.forEach(x => { r.push(x); a.length = 2; }); return r; });
    run('filter deletes', () => { const a = [1, 2, 3, 4]; return own(a.filter((x, i) => { delete a[i + 1]; return true; })); });
    run('reduce writes', () => { const a = [1, 2, 3]; return a.reduce((s, x, i) => { a[i + 1] = 10; return s + x; }, 0); });
    run('frozen fill', () => Object.freeze([1, 2]).fill(0));
    run('sealed fill', () => own(Object.seal([1, 2]).fill(0)));
    run('nonext fill hole', () => own(Object.preventExtensions([1, , 3]).fill(0)));
    Array.prototype[1] = 'P';
    for (const m of ['indexOf', 'lastIndexOf', 'includes']) run('proto ' + m, () => [1, , 3][m]('P'));
    run('proto concat', () => own([1, , 3].concat([4, , 6])));
    run('proto copies', () => [own([1, , 3].toReversed()), own([1, , 3].slice()), own([1, , 3].map(x => x)), own([1, , 3].with(0, 0))]);
    delete Array.prototype[1];
    Object.prototype[1] = 'O';
    run('objproto copies', () => [own([1, , 3].slice()), [1, , 3].indexOf('O'), own([1, , 3].concat([]))]);
    delete Object.prototype[1];
    class Sub extends Array {}
    run('sub', () => { const s = Sub.of(3, 1, 2); return [s.map(x => x) instanceof Sub, s.concat([1]) instanceof Sub, s.toSorted() instanceof Sub, s.filter(Boolean).length]; });
    function args() { return arguments; }
    run('args', () => { const a = args(1, 2, 3); return [[].indexOf.call(a, 2), [].lastIndexOf.call(a, 1), own([].concat.call(a, [4])), own([].fill.call(args(1, 2), 0)), own([].toReversed.call(args(1, 2))), [].includes.call(a, 3)]; });
    run('arraylike', () => { const al = { length: 3, 0: 'a', 2: 'c' }; return [[].indexOf.call(al, 'c'), own([].toReversed.call(al)), own([].concat.call(al)), own(Array.prototype.fill.call({ length: 2 }, 1))]; });
    run('spreadable', () => { const sp = { length: 2, 0: 'x', [Symbol.isConcatSpreadable]: true }; const ns = [1, 2]; ns[Symbol.isConcatSpreadable] = false; return [own([0].concat(sp)), [0].concat(ns).length]; });
    run('proxy', () => { const p = new Proxy([1, 2, 3], {}); return [[].indexOf.call(p, 3), own([].toReversed.call(p)), own([0].concat(p))]; });
    run('with getter', () => { let reads = 0; const a = [1, 2, 3]; Object.defineProperty(a, 1, { get() { reads++; return 2; } }); a.with(1, 'w'); return reads; });
    console.log(out.join('\n'));
  }, "plain indexOf [1,-1,-1,-1,-1,-1,-1,-1,-1,-1,4]\nplain lastIndexOf [3,0,-1,-1,-1,-1,4]\nplain fill [5,\"0,1,2,3,4\"]\nplain fillAll [5,\"0,1,2,3,4\"]\nplain concat [14,\"0,1,2,3,4,5,6,7,8,9,10,11,13\"]\nplain toReversed [5,\"0,1,2,3,4\"]\nplain toSorted [5,\"0,1,2,3,4\"]\nplain toSpliced [4,\"0,1,2,3\"]\nplain with [5,\"0,1,2,3,4\"]\nplain map [5,\"0,1,2,3,4\"]\nplain filter [3,\"0,1,2\"]\nplain forEach [0,1,2,3,4]\nplain reduce \":0:1:2:3:4\"\nplain some/every [true,true]\nplain slice [3,\"0,1,2\"]\nplain splice [[2,\"0,1\"],[3,\"0,1,2\"]]\nplain flat [6,\"0,1,2,3,4,5\"]\nholey indexOf [-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]\nholey lastIndexOf [-1,0,-1,-1,-1,-1,0]\nholey fill [5,\"0,1,2,4\"]\nholey fillAll [5,\"0,1,2,3,4\"]\nholey concat [14,\"0,2,4,5,7,9,10,11,13\"]\nholey toReversed [5,\"0,1,2,3,4\"]\nholey toSorted [5,\"0,1,2,3,4\"]\nholey toSpliced [4,\"0,1,2,3\"]\nholey with [5,\"0,1,2,3,4\"]\nholey map [5,\"0,2,4\"]\nholey filter [2,\"0,1\"]\nholey forEach [0,2,4]\nholey reduce \":0:2:4\"\nholey some/every [true,true]\nholey slice [3,\"1\"]\nholey splice [[2,\"1\"],[3,\"0,2\"]]\nholey flat [4,\"0,1,2,3\"]\nmixed indexOf [-1,-1,-1,1,1,3,4,5,6,7,-1]\nmixed lastIndexOf [-1,-1,-1,2,5,7,-1]\nmixed fill [8,\"0,1,2,3,4,5,6,7\"]\nmixed fillAll [8,\"0,1,2,3,4,5,6,7\"]\nmixed concat [20,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,19\"]\nmixed toReversed [8,\"0,1,2,3,4,5,6,7\"]\nmixed toSorted [8,\"0,1,2,3,4,5,6,7\"]\nmixed toSpliced [7,\"0,1,2,3,4,5,6\"]\nmixed with [8,\"0,1,2,3,4,5,6,7\"]\nmixed map [8,\"0,1,2,3,4,5,6,7\"]\nmixed filter [8,\"0,1,2,3,4,5,6,7\"]\nmixed forEach [0,1,2,3,4,5,6,7]\nmixed reduce \":0:1:2:3:4:5:6:7\"\nmixed some/every [false,true]\nmixed slice [3,\"0,1,2\"]\nmixed splice [[2,\"0,1\"],[6,\"0,1,2,3,4,5\"]]\nmixed flat [9,\"0,1,2,3,4,5,6,7,8\"]\nbig indexOf [2,3,-1,0,0,-1,-1,-1,-1,-1,-1]\nbig lastIndexOf [37,1,-1,35,-1,-1,36]\nbig fill [40,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39\"]\nbig fillAll [40,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39\"]\nbig concat [84,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,83\"]\nbig toReversed [40,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39\"]\nbig toSorted [40,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39\"]\nbig toSpliced [39,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38\"]\nbig with [40,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39\"]\nbig map [40,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39\"]\nbig filter [34,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33\"]\nbig forEach [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39]\nbig reduce \":0:1:2:3:4:5:6:7:8:9:10:11:12:13:14:15:16:17:18:19:20:21:22:23:24:25:26:27:28:29:30:31:32:33:34:35:36:37:38:39\"\nbig some/every [true,true]\nbig slice [3,\"0,1,2\"]\nbig splice [[2,\"0,1\"],[38,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37\"]]\nbig flat [41,\"0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40\"]\nmap grows [3,\"0,1,2\"]\nforEach shrinks [1,2]\nfilter deletes [2,\"0,1\"]\nreduce writes 21\nfrozen fill \"THROW TypeError\"\nsealed fill [2,\"0,1\"]\nnonext fill hole \"THROW TypeError\"\nproto indexOf 1\nproto lastIndexOf 1\nproto includes true\nproto concat [6,\"0,1,2,3,4,5\"]\nproto copies [[3,\"0,1,2\"],[3,\"0,1,2\"],[3,\"0,1,2\"],[3,\"0,1,2\"]]\nobjproto copies [[3,\"0,1,2\"],1,[3,\"0,1,2\"]]\nsub [true,true,false,3]\nargs [1,0,[2,\"0,1\"],{\"0\":0,\"1\":0},[2,\"0,1\"],true]\narraylike [2,[3,\"0,1,2\"],[1,\"0\"],{\"0\":1,\"1\":1,\"length\":2}]\nspreadable [[3,\"0,1\"],2]\nproxy [2,[3,\"0,1,2\"],[4,\"0,1,2,3\"]]\nwith getter 0\n"],
  [function holes() {
    const h = () => [1, , 3];
    const show = a => JSON.stringify([a, a.length, 1 in a]);
    const out = [show(h().toReversed()), show(h().with(0, 9)), show(h().toSorted()), show(h().toSpliced(0, 0)), show(h().concat(h())),
      show(h().fill(7, 1, 2)), [h().indexOf(undefined), h().lastIndexOf(undefined), h().includes(undefined)]];
    Array.prototype[1] = 'P';
    out.push(show(h().toReversed()), show(h().concat([])), h().indexOf('P'), show(h().slice()));
    delete Array.prototype[1];
    console.log(out.join('\n'));
  }, "[[3,null,1],3,true]\n[[9,null,3],3,true]\n[[1,3,null],3,true]\n[[1,null,3],3,true]\n[[1,null,3,1,null,3],6,false]\n[[1,7,3],3,true]\n-1,-1,true\n[[3,\"P\",1],3,true]\n[[1,\"P\",3],3,true]\n1\n[[1,\"P\",3],3,true]\n"],
  [function species() {
    const out = [];
    const a = [1, 2, 3];
    out.push(a.map(x => x).constructor === Array, Array.of(7, 8).length, Array.of.call(Array, 1, 2, 3).length);
    class Sub extends Array {}
    const s = Sub.from([1, 2, 3]);
    out.push(s.map(x => x) instanceof Sub, s.filter(Boolean) instanceof Sub, s.slice() instanceof Sub);
    const own = [1, 2]; own.constructor = function Fake() { return { fake: true }; }; own.constructor[Symbol.species] = undefined;
    out.push(own.map(x => x).constructor === Array);
    const own2 = [1, 2]; own2.constructor = { [Symbol.species]: function (n) { this.made = n; } };
    out.push(JSON.stringify(own2.slice()));
    const saved = Object.getOwnPropertyDescriptor(Array, Symbol.species);
    let calls = 0;
    Object.defineProperty(Array, Symbol.species, { get() { calls++; return Array; }, configurable: true });
    [1].map(x => x); [1].filter(Boolean); [1].slice(); out.push(calls);
    Object.defineProperty(Array, Symbol.species, saved);
    let ctorReads = 0;
    const savedCtor = Object.getOwnPropertyDescriptor(Array.prototype, 'constructor');
    Object.defineProperty(Array.prototype, 'constructor', { get() { ctorReads++; return Array; }, configurable: true });
    [1].concat([2]); [1].splice(0, 0); out.push(ctorReads);
    Object.defineProperty(Array.prototype, 'constructor', savedCtor);
    console.log(JSON.stringify(out));
    const out2 = [];
    const bad = [1]; bad.constructor = 5;
    try { bad.map(x => x); out2.push('no throw'); } catch (e) { out2.push(e.name); }
    const nul = [1, 2]; nul.constructor = { [Symbol.species]: null }; out2.push(Array.isArray(nul.slice()));
    out2.push(JSON.stringify([1, , 3].map(x => x * 2)), [1, , 3].map(x => x).length, 1 in [1, , 3].map(x => x));
    out2.push(JSON.stringify([1, 2, 3, 4].splice(1, 2)), JSON.stringify([1, , 3].splice(0, 3)), JSON.stringify([1, 2, 3].slice(1)));
    const big = Array.from({ length: 40 }, (_, k) => k); out2.push(big.slice(3).length, big.map(x => x).length, big.splice(0, 30).length);
    class S2 extends Array {} ; const s2 = S2.of(1, 2, 3); out2.push(s2.slice(1).length, s2.map(x => x).length);
    console.log(JSON.stringify(out2));
  }, "[true,2,3,true,true,true,true,\"{\\\"0\\\":1,\\\"1\\\":2,\\\"made\\\":2,\\\"length\\\":2}\",3,2]\n[\"TypeError\",true,\"[2,null,6]\",3,false,\"[2,3]\",\"[1,null,3]\",\"[2,3]\",37,40,30,2,3]\n"],
  [function iterpatch() {
    const out = [];
    const orig = Array.prototype[Symbol.iterator];
    Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };
    out.push([...[1, 2]], Array.from([1, 2]), (function () { return [...arguments]; })(1, 2));
    Array.prototype[Symbol.iterator] = orig;
    const ArrayIterProto = Object.getPrototypeOf([][Symbol.iterator]());
    const next = ArrayIterProto.next;
    ArrayIterProto.next = function () { const r = next.call(this); if (!r.done) r.value = r.value * 10; return r; };
    out.push([...[1, 2]], Array.from([1, 2]));
    ArrayIterProto.next = next;
    const own = [1, 2]; own[Symbol.iterator] = function* () { yield 'own'; };
    out.push([...own], Array.from(own), [...[1, , 3]], Array.from([1, , 3]));
    console.log(JSON.stringify(out));
  }, "[[\"patched\"],[\"patched\"],[1,2],[10,20],[10,20],[\"own\"],[\"own\"],[1,null,3],[1,null,3]]\n"],
  [function forof() {
    const out = [Array.prototype[Symbol.iterator] === Array.prototype.values, (function () { return arguments[Symbol.iterator] === Array.prototype.values; })()];
    const run = a => { const r = []; for (const x of a) r.push(x); return r; };
    const orig = Array.prototype[Symbol.iterator];
    Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };
    out.push(run([1, 2]));
    Array.prototype[Symbol.iterator] = orig;
    const P = Object.getPrototypeOf([][Symbol.iterator]()); const next = P.next;
    P.next = function () { const r = next.call(this); if (!r.done) r.value *= 10; return r; };
    out.push(run([1, 2]));
    P.next = next;
    for (let i = 0; i < 3000; i++) run([1, 2]);
    P.next = function () { const r = next.call(this); if (!r.done) r.value *= 10; return r; };
    out.push(run([1, 2]));
    P.next = next;
    console.log(JSON.stringify(out));
  }, "[true,true,[\"patched\"],[10,20],[10,20]]\n"],
  [function protjit() {
    const out = [];
    const run = a => { const r = []; for (const x of a) r.push(x); return r; };
    function setNext(o, f) { o.next = f; }
    function setIter(o, f) { o[Symbol.iterator] = f; }
    for (let i = 0; i < 20000; i++) { setNext({ next: 1 }, 2); setIter({}, null); run([1]); [...[1]]; }
    const P = Object.getPrototypeOf([][Symbol.iterator]()); const next = P.next;
    setNext(P, function () { const r = next.call(this); if (!r.done) r.value = 'n' + r.value; return r; });
    out.push(run([1, 2]), [...[1, 2]], Array.from([1, 2]));
    setNext(P, next);
    const arr = [1, 2];
    setIter(arr, function* () { yield 'own'; });
    out.push(run(arr), [...arr]);
    console.log(JSON.stringify(out));
  }, "[[\"n1\",\"n2\"],[\"n1\",\"n2\"],[\"n1\",\"n2\"],[\"own\"],[\"own\"]]\n"],
  [function speciesjit() {
    function Fake() { this.fake = true; }
    Fake[Symbol.species] = Fake;
    function setCtor(a, c) { a.constructor = c; return a; }
    function setCtor2(a, c) { a.constructor = c; return a; }
    for (let i = 0; i < 20000; i++) { setCtor([1], Array); setCtor2({ constructor: 1 }, Array); }
    const a = setCtor([1, 2], Fake);
    const b = setCtor2(Object.assign([1, 2], { constructor: 0 }), Fake);
    console.log(JSON.stringify([a.map(x => x).fake === true, b.map(x => x).fake === true]));
  }, "[true,true]\n"],
];

let failures = 0;
for (const [section, expected] of sections) {
  const child = spawnSync(process.execPath, ['-e', `(${section})()`], { encoding: 'utf8', timeout: 60000 });
  if (child.status !== 0 || child.stdout !== expected) {
    failures++;
    console.log(`FAIL ${section.name} (status ${child.status})\n--- got\n${child.stdout}${child.stderr}\n--- expected\n${expected}`);
  }
}
if (failures) throw new Error(`${failures} array builtin sections differ from Node`);
console.log('PASS array builtins match Node across dense fast paths');
