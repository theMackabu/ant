// Array builtins take dense fast paths (direct buffer reads, a species protector
// for %Array%, an iteration protector for spread/for-of/Array.from, prepared
// callbacks). Each section runs in a fresh process so the protectors start
// valid, and must print what Node prints, including when getters collect
// garbage while values are being copied.
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
  [function flatmap() {
    const a = [1, 2, 3, 4, 5, 6, 7, 8];
    const r1 = a.flatMap((x, i) => { if (i === 0) for (let k = 0; k < 100; k++) a.push(k); [[9, 9, 9, 9, 9, 9, 9, 9]]; return [x]; });
    const b = [1, 2, 3, 4];
    const r2 = b.flatMap((x, i) => { if (i === 0) b.length = 1; return [x]; });
    const c = [1, 2, 3];
    const r3 = c.flatMap((x, i) => { if (i === 0) c[2] = 'changed'; return x; });
    console.log(JSON.stringify([r1.slice(0, 8), r1.length, r2, r3]));
  }, "[[1,2,3,4,5,6,7,8],8,[1],[1,2,\"changed\"]]\n"],
  [function groupby() {
    const out = [];
    out.push(Object.groupBy([1, 2, 3, 4], x => x % 2 ? 'odd' : 'even'));
    out.push(Object.groupBy(new Set([1, 2, 3]), x => x > 1 ? 'big' : 'small'));
    out.push(Object.groupBy((function* () { yield 'a'; yield 'bb'; })(), s => s.length));
    out.push(Object.groupBy('abca', c => c));
    out.push(Object.groupBy([1, , 3], x => String(x)));
    try { Object.groupBy({ length: 2, 0: 'x', 1: 'y' }, x => x); out.push('no throw'); } catch (e) { out.push(e.name); }
    const m = Map.groupBy([1, 2, 3], x => x % 2); out.push([...m.entries()]);
    console.log(JSON.stringify(out));
  }, "[{\"odd\":[1,3],\"even\":[2,4]},{\"small\":[1],\"big\":[2,3]},{\"1\":[\"a\"],\"2\":[\"bb\"]},{\"a\":[\"a\",\"a\"],\"b\":[\"b\"],\"c\":[\"c\"]},{\"1\":[1],\"3\":[3],\"undefined\":[null]},\"TypeError\",[[1,[1,3]],[0,[2]]]]\n"],
  [function iterusers() {
    const out = [];
    const t = f => { try { return f(); } catch (e) { return e.constructor.name; } };
    out.push(t(() => Object.fromEntries([['a', 1]])), t(() => Object.fromEntries(5)), t(() => Object.fromEntries(null)), t(() => Object.fromEntries(new Map([['k', 2]]))));
    out.push(t(() => Array.from(new Set([1, 2]))), t(() => Array.from('héllo')), t(() => Array.from({ length: 2, 0: 'x' })), t(() => Array.from(5)));
    let getterRuns = 0; const withGetter = { get [Symbol.iterator]() { getterRuns++; return [][Symbol.values] || function* () { yield 1; }; } };
    out.push(t(() => Array.from(withGetter)), getterRuns);
    const a = [1, 2, 3, 4, 5, 6, 7, 8];
    out.push(a.flatMap((x, i) => { if (i === 0) for (let k = 0; k < 100; k++) a.push(k); [[9, 9, 9, 9, 9, 9, 9, 9]]; return [x]; }).slice(0, 8));
    const b = [1, 2, 3, 4]; out.push(b.flatMap((x, i) => { if (i === 0) b.length = 1; return [x]; }));
    out.push((function () { return [[1], arguments].flat().length; })(7, 8), [[1, [2]], [3]].flat(2), [1, , 3].flatMap(x => [x, x]));
    Promise.all('ab').then(v => out.push(v)).then(() => Promise.all(5)).catch(e => out.push(e.constructor.name))
      .then(() => Promise.race([Promise.resolve('r')])).then(v => out.push(v)).then(() => console.log(JSON.stringify(out)));
  }, "[{\"a\":1},\"TypeError\",\"TypeError\",{\"k\":2},[1,2],[\"h\",\"\u00e9\",\"l\",\"l\",\"o\"],[\"x\",null],[],[1],1,[1,2,3,4,5,6,7,8],[1],2,[1,2,3],[1,1,3,3],[\"a\",\"b\"],\"TypeError\",\"r\"]\n"],
  [function promiter() {
    const out = [];
    const settle = p => p.then(v => ['ok', v], e => ['err', e.constructor.name, e.errors ? e.errors.length : undefined]);
    (async () => {
      for (const m of ['all', 'allSettled', 'race', 'any']) for (const input of ['ab', 5, null, undefined, [Promise.resolve(1)], new Set([2])]) out.push([m, String(input), await settle(Promise[m](input))]);
      console.log(JSON.stringify(out));
    })();
  }, "[[\"all\",\"ab\",[\"ok\",[\"a\",\"b\"]]],[\"all\",\"5\",[\"err\",\"TypeError\",null]],[\"all\",\"null\",[\"err\",\"TypeError\",null]],[\"all\",\"undefined\",[\"err\",\"TypeError\",null]],[\"all\",\"[object Promise]\",[\"ok\",[1]]],[\"all\",\"[object Set]\",[\"ok\",[2]]],[\"allSettled\",\"ab\",[\"ok\",[{\"status\":\"fulfilled\",\"value\":\"a\"},{\"status\":\"fulfilled\",\"value\":\"b\"}]]],[\"allSettled\",\"5\",[\"err\",\"TypeError\",null]],[\"allSettled\",\"null\",[\"err\",\"TypeError\",null]],[\"allSettled\",\"undefined\",[\"err\",\"TypeError\",null]],[\"allSettled\",\"[object Promise]\",[\"ok\",[{\"status\":\"fulfilled\",\"value\":1}]]],[\"allSettled\",\"[object Set]\",[\"ok\",[{\"status\":\"fulfilled\",\"value\":2}]]],[\"race\",\"ab\",[\"ok\",\"a\"]],[\"race\",\"5\",[\"err\",\"TypeError\",null]],[\"race\",\"null\",[\"err\",\"TypeError\",null]],[\"race\",\"undefined\",[\"err\",\"TypeError\",null]],[\"race\",\"[object Promise]\",[\"ok\",1]],[\"race\",\"[object Set]\",[\"ok\",2]],[\"any\",\"ab\",[\"ok\",\"a\"]],[\"any\",\"5\",[\"err\",\"TypeError\",null]],[\"any\",\"null\",[\"err\",\"TypeError\",null]],[\"any\",\"undefined\",[\"err\",\"TypeError\",null]],[\"any\",\"[object Promise]\",[\"ok\",1]],[\"any\",\"[object Set]\",[\"ok\",2]]]\n"],
  [function reduce() {
    const t = f => { try { return f(); } catch (e) { return e.constructor.name; } };
    const add = (a, b) => String(a) + ',' + String(b);
    console.log(JSON.stringify([t(() => [, 1, , 2, ,].reduce(add)), t(() => [, 1, , 2, ,].reduceRight(add)), t(() => [, ,].reduce(add)), t(() => [, ,].reduceRight(add)), t(() => [].reduce(add)), t(() => [5].reduceRight(add)), t(() => [1, 2, 3].reduceRight(add, 'i'))]));
  }, "[\"1,2\",\"2,1\",\"TypeError\",\"TypeError\",\"TypeError\",5,\"i,3,2,1\"]\n"],
  [function callbacks() {
    const out = [];
    const t = f => { try { return f(); } catch (e) { return 'THROW ' + e.constructor.name; } };
    const runAll = tag => {
      const a = () => [1, 2, 3, 4];
      out.push(tag + ' mutate-push ' + JSON.stringify(t(() => { const x = a(); return x.map((v, i) => { if (i === 0) x.push(9); return v * 2; }); })));
      out.push(tag + ' mutate-shrink ' + JSON.stringify(t(() => { const x = a(); const r = []; x.forEach(v => { r.push(v); x.length = 2; }); return r; })));
      out.push(tag + ' mutate-delete ' + JSON.stringify(t(() => { const x = a(); return x.filter((v, i) => { delete x[i + 1]; return true; }); })));
      out.push(tag + ' mutate-holes ' + JSON.stringify(t(() => { const x = a(); return x.reduce((s, v, i) => { x[i + 2] = 'w'; return s + v; }, ''); })));
      out.push(tag + ' throw ' + JSON.stringify(t(() => a().some(v => { if (v === 3) throw new TypeError('x'); return false; }))));
      out.push(tag + ' this ' + JSON.stringify(t(() => { const o = { k: 5 }; return [a().map(function () { return this && this.k; }, o), a().map(function () { 'use strict'; return this; }), a().map(() => typeof this)]; })));
      out.push(tag + ' bound ' + JSON.stringify(t(() => a().map(function (v, i) { return [this.b, v, i]; }.bind({ b: 1 }, 'bv')))));
      out.push(tag + ' builtin ' + JSON.stringify(t(() => [a().map(String), a().filter(Boolean).length, ['1', '2'].map(Number), a().every(Number.isInteger)])));
      class B { m(v) { return 'B' + v; } }
      class C extends B { run(arr) { return arr.map(v => super.m(v)); } }
      out.push(tag + ' super ' + JSON.stringify(t(() => new C().run(a()))));
      out.push(tag + ' async ' + JSON.stringify(t(() => a().map(async v => v).map(p => p instanceof Promise))));
      out.push(tag + ' generator ' + JSON.stringify(t(() => a().map(function* (v) { yield v; }).map(g => g.next().value))));
      out.push(tag + ' args-count ' + JSON.stringify(t(() => a().map(function () { return arguments.length; }))));
      out.push(tag + ' find-holes ' + JSON.stringify(t(() => { const seen = []; [1, , 3].find((v, i) => { seen.push([v, i]); }); return seen; })));
      out.push(tag + ' reentrant ' + JSON.stringify(t(() => a().map(v => a().map(w => v * w).reduce((s, x) => s + x)))));
      let depth = 0; const deep = () => { depth++; [1].forEach(deep); };
      out.push(tag + ' overflow ' + JSON.stringify([t(deep), depth > 100]));
      out.push(tag + ' flatMap ' + JSON.stringify(t(() => a().flatMap(v => v % 2 ? [v, [v]] : []))));
      out.push(tag + ' groupBy ' + JSON.stringify(t(() => Object.groupBy(a(), v => v % 2))));
    };
    runAll('cold');
    for (let i = 0; i < 300; i++) runAll('warm');
    out.length = 0;
    runAll('hot');
    console.log(out.join('\n'));
  }, "hot mutate-push [2,4,6,8]\nhot mutate-shrink [1,2]\nhot mutate-delete [1,3]\nhot mutate-holes \"12ww\"\nhot throw \"THROW TypeError\"\nhot this [[5,5,5,5],[null,null,null,null],[\"object\",\"object\",\"object\",\"object\"]]\nhot bound [[1,\"bv\",1],[1,\"bv\",2],[1,\"bv\",3],[1,\"bv\",4]]\nhot builtin [[\"1\",\"2\",\"3\",\"4\"],4,[1,2],true]\nhot super [\"B1\",\"B2\",\"B3\",\"B4\"]\nhot async [true,true,true,true]\nhot generator [1,2,3,4]\nhot args-count [3,3,3,3]\nhot find-holes [[1,0],[null,1],[3,2]]\nhot reentrant [10,20,30,40]\nhot overflow [\"THROW RangeError\",true]\nhot flatMap [1,[1],3,[3]]\nhot groupBy {\"0\":[2,4],\"1\":[1,3]}\n"],
  [function gcrepros() {
    const churn = () => { const j = []; for (let k = 0; k < 2000; k++) j.push({ k, s: 'x' + k }); };
    const bad = arr => arr.filter(v => !v || v.payload !== 'v' + v.id).length;
    const getters = (n, mk) => { const a = new Array(n).fill(0); for (let i = 0; i < n; i++) Object.defineProperty(a, i, { get() { churn(); return mk(i); } }); return a; };
    const obj = i => ({ id: i, payload: 'v' + i });
    const out = {};
    for (const m of ['toReversed', 'toSorted', 'toSpliced', 'with']) {
      const a = getters(48, obj);
      const r = m === 'toSorted' ? a.toSorted((x, y) => x.id - y.id) : m === 'toSpliced' ? a.toSpliced(0, 0) : m === 'with' ? a.with(0, obj(0)) : a.toReversed();
      out[m] = bad(r);
    }
    out.applyArguments = bad((function () { return [...arguments]; }).apply(null, getters(48, obj)));
    out.reflectApply = bad(Reflect.apply((...r) => r, null, getters(48, obj)));
    out.pushApply = (() => { const g = []; Array.prototype.push.apply(g, getters(48, obj)); return bad(g); })();
    class K { constructor(...r) { this.r = r; } }
    out.reflectConstruct = bad(Reflect.construct(K, getters(48, obj)).r);
    out.spread = bad([...getters(48, obj)]);
    out.from = bad(Array.from(getters(48, obj)));
    out.concat = bad([].concat(getters(48, obj)));
    out.sortGenerated = (() => { const a = getters(32, obj); const plain = Array.prototype.slice.call(a); plain.sort((x, y) => { churn(); return y.id - x.id; }); return bad(plain); })();
    console.log(JSON.stringify(out));
  }, "{\"toReversed\":0,\"toSorted\":0,\"toSpliced\":0,\"with\":0,\"applyArguments\":0,\"reflectApply\":0,\"pushApply\":0,\"reflectConstruct\":0,\"spread\":0,\"from\":0,\"concat\":0,\"sortGenerated\":0}\n"],
  [function sortsem() {
    const show = a => JSON.stringify([a, a.length, Object.keys(a).join()]);
    const out = [];
    out.push(show([3, , 1].sort()), show([3, undefined, , 1].sort()), show([3, undefined, , 1].sort((a, b) => b - a)));
    { const a = Array.from({ length: 10 }, (_, i) => 9 - i); let first = true; a.sort((x, y) => { if (first) { first = false; a.length = 0; } return x - y; }); out.push(show(a)); }
    { const a = [5, 4, 3, 2, 1]; let first = true; a.sort((x, y) => { if (first) { first = false; for (let k = 0; k < 50; k++) a.push(99); } return x - y; }); out.push(JSON.stringify([a.slice(0, 7), a.length])); }
    Array.prototype[1] = 'P'; out.push(show([3, , 1].sort())); delete Array.prototype[1];
    out.push(show(Array.prototype.sort.call({ length: 3, 0: 'c', 2: 'a' })));
    out.push(show((function () { return [].sort.call(arguments); })(3, 1, 2)));
    out.push(show(Object.freeze([1]).sort()));
    try { Object.freeze([2, 1]).sort(); out.push('no throw'); } catch (e) { out.push(e.constructor.name); }
    out.push(show([10, 9, 1, 2].sort()), show(['b', 'a', 'B'].sort()), show([2, 1].toSorted()), show([3, , 1].toSorted()));
    console.log(out.join('\n'));
  }, "[[1,3,null],3,\"0,1\"]\n[[1,3,null,null],4,\"0,1,2\"]\n[[3,1,null,null],4,\"0,1,2\"]\n[[0,1,2,3,4,5,6,7,8,9],10,\"0,1,2,3,4,5,6,7,8,9\"]\n[[1,2,3,4,5,99,99],55]\n[[1,3,\"P\"],3,\"0,1,2\"]\n[{\"0\":\"a\",\"1\":\"c\",\"length\":3},3,\"0,1,length\"]\n[{\"0\":1,\"1\":2,\"2\":3},3,\"0,1,2\"]\n[[1],1,\"0\"]\nTypeError\n[[1,10,2,9],4,\"0,1,2,3\"]\n[[\"B\",\"a\",\"b\"],3,\"0,1,2\"]\n[[1,2],2,\"0,1\"]\n[[1,3,null],3,\"0,1,2\"]\n"],
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
if (failures) throw new Error(`${failures} array builtin sections differ from Node`);
console.log('PASS array builtins match Node across dense fast paths');
