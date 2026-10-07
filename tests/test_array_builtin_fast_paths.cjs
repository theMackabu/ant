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
  [function sortints() {
    const pick = [0, -0, 1, -1, 9, 10, 11, 19, 99, 100, 101, -9, -10, -11, -100, 2147483647, -2147483648, 214748364, -214748364, 1000000000, 999999999, 12, 120, 1200, 121, 13, 2, 20, 200];
    let rnd = 7; const r = () => (rnd = (rnd * 1103515245 + 12345) % 2147483648);
    const big = Array.from({ length: 3000 }, (_, i) => i % 3 === 0 ? pick[i % pick.length] : (r() % 2 ? r() : -r()) % 2147483648 | 0);
    const strs = Array.from({ length: 500 }, () => String.fromCharCode(97 + r() % 26) + (r() % 3 ? '\\0x' : '') + r() % 100);
    const nul = ['a\0b', 'a\0a', 'a', 'a\0', 'b'];
    console.log(JSON.stringify([pick.slice().sort(), big.slice().sort().slice(0, 50), big.slice().sort().join().length, strs.slice().sort().slice(0, 20), nul.slice().sort()]));
  }, "[[-1,-10,-100,-11,-214748364,-2147483648,-9,0,0,1,10,100,1000000000,101,11,12,120,1200,121,13,19,2,20,200,214748364,2147483647,9,99,999999999],[-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-10,-10,-10,-10,-10,-10,-10,-10,-10,-10,-10,-10,-10,-10,-10],27725,[\"a0\",\"a16\",\"a20\",\"a20\",\"a20\",\"a28\",\"a32\",\"a4\",\"a40\",\"a44\",\"a44\",\"a56\",\"a64\",\"a68\",\"a84\",\"a92\",\"a\\\\0x16\",\"a\\\\0x24\",\"a\\\\0x24\",\"a\\\\0x24\"],[\"a\",\"a\\u0000\",\"a\\u0000a\",\"a\\u0000b\",\"b\"]]\n"],
  [function sortcmp() {
    const t = f => { try { return JSON.stringify(f()); } catch (e) { return e.constructor.name + ':' + e.message; } };
    console.log([
      t(() => [3, 1, 2].sort((a, b) => String(a - b))),
      t(() => [3, 1, 2].sort((a, b) => ({ valueOf: () => a - b }))),
      t(() => [3, 1, 2].sort(() => { throw new RangeError('cmp'); })),
      t(() => [3, 1, 2].sort((a, b) => NaN)),
      t(() => [10, 9, 1, -1, -10, 0, -0, 2.5, 100, 21, 3e21, -2147483648, 2147483647, 4294967296].sort()),
      t(() => [5, 'a', 3, 'B', 10].sort()),
      t(() => [1, 1n, 0, -1n].sort()),
    ].join('\n'));
  }, "[1,2,3]\n[1,2,3]\nRangeError:cmp\n[3,1,2]\n[-1,-10,-2147483648,0,0,1,10,100,2.5,21,2147483647,3e+21,4294967296,9]\n[10,3,5,\"B\",\"a\"]\nTypeError:Do not know how to serialize a BigInt\n"],
  [function speciesresult() {
    const holder = [];
    for (let i = 0; i < 64; i++) holder.push([0, 1, 2, 3, 4, 5, 6, 7]);
    for (let r = 0; r < 20; r++) for (let i = 0; i < 300000; i++) ({ i });
    const sliceInto = (target, round) => {
      const src = Array.from({ length: 8 }, (_, i) => ({ v: round * 8 + i, pad: [round, i] }));
      src.constructor = { [Symbol.species]: function () { return target; } };
      src.slice(0, 8);
    };
    let bad = 0;
    for (let round = 0; round < 120; round++) {
      const target = holder[round % 64];
      target.length = 0;
      sliceInto(target, round);
      for (let i = 0; i < 400000; i++) ({ junk: i, more: [i] });
      for (let i = 0; i < 8; i++) {
        const o = target[i];
        if (!o || o.v !== round * 8 + i || !Array.isArray(o.pad) || o.pad[1] !== i) { bad++; break; }
      }
    }
    const out = ['old species target lost values: ' + bad];
    const using = (result, fn) => {
      class S extends Array { static get [Symbol.species]() { return function () { return result; }; } }
      try { fn(S.from([1, 2, 3])); return 'ok ' + result.join(); } catch (e) { return e.constructor.name; }
    };
    const ops = { slice: a => a.slice(0, 2), concat: a => a.concat([9]), splice: a => a.splice(0, 2), map: a => a.map(x => x), filter: a => a.filter(() => true), flat: a => a.flat(), flatMap: a => a.flatMap(x => [x]) };
    for (const [name, fn] of Object.entries(ops)) {
      out.push(name + ' ' + ['freeze', 'seal', 'preventExtensions'].map(lock => using(Object[lock]([]), fn)).join(' / ') + ' / existing ' + using(Object.preventExtensions([0, 0]), fn));
    }
    let calls = 0;
    using(Object.freeze([]), a => a.map(x => (calls++, x)));
    out.push('map callback calls before throwing: ' + calls);
    console.log(out.join('\n'));
  }, "old species target lost values: 0\nslice TypeError / TypeError / TypeError / existing ok 1,2\nconcat TypeError / TypeError / TypeError / existing TypeError\nsplice TypeError / TypeError / TypeError / existing ok 1,2\nmap TypeError / TypeError / TypeError / existing TypeError\nfilter TypeError / TypeError / TypeError / existing TypeError\nflat TypeError / TypeError / TypeError / existing TypeError\nflatMap TypeError / TypeError / TypeError / existing TypeError\nmap callback calls before throwing: 1\n"],
  [function packedresults() {
    const show = a => JSON.stringify([a.length, Object.keys(a).join(), a.join('|'), a.includes(undefined), a.indexOf(undefined), a.flat().length]);
    const holey = [1, , 3, undefined, null];
    const sparse = []; sparse[3000] = 'x';
    const out = [
      holey.map(x => x), holey.slice(), holey.slice(1, 3), [1, , 3, 4].splice(0, 3), [1, 2, 3, 4].splice(1, 2),
      [1, 2, 3].map(x => x * 2), [[1], [2, [3]]].slice(), sparse.map(x => x), sparse.slice(2990),
      Array.prototype.slice.call({ length: 3, 0: 'a', 2: 'c' }), Array.from({ length: 2 }).slice(),
      [true, false, null, undefined, 1.5, -0, 'é'].map(x => x),
    ].map(show);
    const victim = [1, 2, 3, 4].map(x => x);
    victim[1] = { toString() { victim.length = 2; victim.push('late'); return 'B'; } };
    out.push(victim.join('-'));
    const holeProto = [1, , 3].map(x => x);
    Array.prototype[1] = 'proto';
    out.push(holeProto.join(), [1, , 3].slice().includes('proto'));
    delete Array.prototype[1];
    console.log(out.join('\n'));
  }, "[5,\"0,2,3,4\",\"1||3||\",true,3,4]\n[5,\"0,2,3,4\",\"1||3||\",true,3,4]\n[2,\"1\",\"|3\",true,-1,1]\n[3,\"0,2\",\"1||3\",true,-1,2]\n[2,\"0,1\",\"2|3\",false,-1,2]\n[3,\"0,1,2\",\"2|4|6\",false,-1,3]\n[2,\"0,1\",\"1|2,3\",false,-1,3]\n[3001,\"3000\",\"||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||x\",true,-1,1]\n[11,\"10\",\"||||||||||x\",true,-1,1]\n[3,\"0,2\",\"a||c\",true,-1,2]\n[2,\"0,1\",\"|\",true,0,2]\n[7,\"0,1,2,3,4,5,6\",\"true|false|||1.5|0|é\",true,3,7]\n1-B-late-\n1,proto,3\ntrue\n"],
  [function arraylikes() {
    const P = Array.prototype, out = [];
    const t = (n, f) => { try { out.push(n + '=' + JSON.stringify(f())); } catch (e) { out.push(n + '!' + e.constructor.name); } };
    const likes = {
      str: () => 'abc', box: () => Object('abc'), newstr: () => new String('héllo'),
      getlen: () => ({ get length() { return 2; }, 0: 'x', 1: 'y' }),
      strlen: () => ({ length: '3', 0: 1, 1: 2, 2: 3 }), fraclen: () => ({ length: 2.7, 0: 'a', 1: 'b', 2: 'c' }),
      protoidx: () => Object.create({ 1: 'p' }, { length: { value: 3 }, 0: { value: 'o', enumerable: true } }),
      getidx: () => ({ length: 2, get 0() { return 'g'; }, 1: 'h' }), fn: () => function (a, b) {}, num: () => 5, nothing: () => null,
    };
    for (const [name, mk] of Object.entries(likes)) {
      t(name + '.map', () => P.map.call(mk(), x => x)); t(name + '.join', () => P.join.call(mk(), '|'));
      t(name + '.includes', () => P.includes.call(mk(), 'b')); t(name + '.indexOf', () => P.indexOf.call(mk(), 'y'));
      t(name + '.filter', () => P.filter.call(mk(), () => true)); t(name + '.reduce', () => P.reduce.call(mk(), (a, b) => a + '' + b, ''));
      t(name + '.slice', () => P.slice.call(mk(), 0)); t(name + '.at', () => P.at.call(mk(), -1)); t(name + '.values', () => [...P.values.call(mk())]);
      t(name + '.toReversed', () => P.toReversed.call(mk())); t(name + '.find', () => P.find.call(mk(), x => x));
      t(name + '.findLast', () => P.findLast.call(mk(), x => x)); t(name + '.entries', () => [...P.entries.call(mk())].length);
    }
    const s = Object('abc');
    t('wrapper', () => [s.length, s[1], 1 in s, 'length' in s, 3 in s]);
    t('at', () => [[1, 2, 3].at(), [1, 2, 3].at('1'), [1, 2, 3].at(-1.5), [1, 2].at({ valueOf: () => 1 })]);
    for (const v of ['', 'a', 'ab', 5]) {
      for (const m of ['push', 'pop', 'shift', 'unshift', 'reverse', 'sort', 'splice', 'copyWithin'])
        t(m + '(' + JSON.stringify(v) + ')', () => typeof P[m].call(v) === 'object' ? 'obj' : 'val');
      t('fill(' + JSON.stringify(v) + ')', () => typeof P.fill.call(v, 'q'));
      t('fillEmpty(' + JSON.stringify(v) + ')', () => typeof P.fill.call(v, 'q', 1, 1));
    }
    console.log(out.join('\n'));
  }, "str.map=[\"a\",\"b\",\"c\"]\nstr.join=\"a|b|c\"\nstr.includes=true\nstr.indexOf=-1\nstr.filter=[\"a\",\"b\",\"c\"]\nstr.reduce=\"abc\"\nstr.slice=[\"a\",\"b\",\"c\"]\nstr.at=\"c\"\nstr.values=[\"a\",\"b\",\"c\"]\nstr.toReversed=[\"c\",\"b\",\"a\"]\nstr.find=\"a\"\nstr.findLast=\"c\"\nstr.entries=3\nbox.map=[\"a\",\"b\",\"c\"]\nbox.join=\"a|b|c\"\nbox.includes=true\nbox.indexOf=-1\nbox.filter=[\"a\",\"b\",\"c\"]\nbox.reduce=\"abc\"\nbox.slice=[\"a\",\"b\",\"c\"]\nbox.at=\"c\"\nbox.values=[\"a\",\"b\",\"c\"]\nbox.toReversed=[\"c\",\"b\",\"a\"]\nbox.find=\"a\"\nbox.findLast=\"c\"\nbox.entries=3\nnewstr.map=[\"h\",\"é\",\"l\",\"l\",\"o\"]\nnewstr.join=\"h|é|l|l|o\"\nnewstr.includes=false\nnewstr.indexOf=-1\nnewstr.filter=[\"h\",\"é\",\"l\",\"l\",\"o\"]\nnewstr.reduce=\"héllo\"\nnewstr.slice=[\"h\",\"é\",\"l\",\"l\",\"o\"]\nnewstr.at=\"o\"\nnewstr.values=[\"h\",\"é\",\"l\",\"l\",\"o\"]\nnewstr.toReversed=[\"o\",\"l\",\"l\",\"é\",\"h\"]\nnewstr.find=\"h\"\nnewstr.findLast=\"o\"\nnewstr.entries=5\ngetlen.map=[\"x\",\"y\"]\ngetlen.join=\"x|y\"\ngetlen.includes=false\ngetlen.indexOf=1\ngetlen.filter=[\"x\",\"y\"]\ngetlen.reduce=\"xy\"\ngetlen.slice=[\"x\",\"y\"]\ngetlen.at=\"y\"\ngetlen.values=[\"x\",\"y\"]\ngetlen.toReversed=[\"y\",\"x\"]\ngetlen.find=\"x\"\ngetlen.findLast=\"y\"\ngetlen.entries=2\nstrlen.map=[1,2,3]\nstrlen.join=\"1|2|3\"\nstrlen.includes=false\nstrlen.indexOf=-1\nstrlen.filter=[1,2,3]\nstrlen.reduce=\"123\"\nstrlen.slice=[1,2,3]\nstrlen.at=3\nstrlen.values=[1,2,3]\nstrlen.toReversed=[3,2,1]\nstrlen.find=1\nstrlen.findLast=3\nstrlen.entries=3\nfraclen.map=[\"a\",\"b\"]\nfraclen.join=\"a|b\"\nfraclen.includes=true\nfraclen.indexOf=-1\nfraclen.filter=[\"a\",\"b\"]\nfraclen.reduce=\"ab\"\nfraclen.slice=[\"a\",\"b\"]\nfraclen.at=\"b\"\nfraclen.values=[\"a\",\"b\"]\nfraclen.toReversed=[\"b\",\"a\"]\nfraclen.find=\"a\"\nfraclen.findLast=\"b\"\nfraclen.entries=2\nprotoidx.map=[\"o\",\"p\",null]\nprotoidx.join=\"o|p|\"\nprotoidx.includes=false\nprotoidx.indexOf=-1\nprotoidx.filter=[\"o\",\"p\"]\nprotoidx.reduce=\"op\"\nprotoidx.slice=[\"o\",\"p\",null]\nprotoidx.at=undefined\nprotoidx.values=[\"o\",\"p\",null]\nprotoidx.toReversed=[null,\"p\",\"o\"]\nprotoidx.find=\"o\"\nprotoidx.findLast=\"p\"\nprotoidx.entries=3\ngetidx.map=[\"g\",\"h\"]\ngetidx.join=\"g|h\"\ngetidx.includes=false\ngetidx.indexOf=-1\ngetidx.filter=[\"g\",\"h\"]\ngetidx.reduce=\"gh\"\ngetidx.slice=[\"g\",\"h\"]\ngetidx.at=\"h\"\ngetidx.values=[\"g\",\"h\"]\ngetidx.toReversed=[\"h\",\"g\"]\ngetidx.find=\"g\"\ngetidx.findLast=\"h\"\ngetidx.entries=2\nfn.map=[null,null]\nfn.join=\"|\"\nfn.includes=false\nfn.indexOf=-1\nfn.filter=[]\nfn.reduce=\"\"\nfn.slice=[null,null]\nfn.at=undefined\nfn.values=[null,null]\nfn.toReversed=[null,null]\nfn.find=undefined\nfn.findLast=undefined\nfn.entries=2\nnum.map=[]\nnum.join=\"\"\nnum.includes=false\nnum.indexOf=-1\nnum.filter=[]\nnum.reduce=\"\"\nnum.slice=[]\nnum.at=undefined\nnum.values=[]\nnum.toReversed=[]\nnum.find=undefined\nnum.findLast=undefined\nnum.entries=0\nnothing.map!TypeError\nnothing.join!TypeError\nnothing.includes!TypeError\nnothing.indexOf!TypeError\nnothing.filter!TypeError\nnothing.reduce!TypeError\nnothing.slice!TypeError\nnothing.at!TypeError\nnothing.values!TypeError\nnothing.toReversed!TypeError\nnothing.find!TypeError\nnothing.findLast!TypeError\nnothing.entries!TypeError\nwrapper=[3,\"b\",true,true,false]\nat=[1,2,3,2]\npush(\"\")!TypeError\npop(\"\")!TypeError\nshift(\"\")!TypeError\nunshift(\"\")!TypeError\nreverse(\"\")=\"obj\"\nsort(\"\")=\"obj\"\nsplice(\"\")!TypeError\ncopyWithin(\"\")=\"obj\"\nfill(\"\")=\"object\"\nfillEmpty(\"\")=\"object\"\npush(\"a\")!TypeError\npop(\"a\")!TypeError\nshift(\"a\")!TypeError\nunshift(\"a\")!TypeError\nreverse(\"a\")=\"obj\"\nsort(\"a\")=\"obj\"\nsplice(\"a\")!TypeError\ncopyWithin(\"a\")!TypeError\nfill(\"a\")!TypeError\nfillEmpty(\"a\")=\"object\"\npush(\"ab\")!TypeError\npop(\"ab\")!TypeError\nshift(\"ab\")!TypeError\nunshift(\"ab\")!TypeError\nreverse(\"ab\")!TypeError\nsort(\"ab\")!TypeError\nsplice(\"ab\")!TypeError\ncopyWithin(\"ab\")!TypeError\nfill(\"ab\")!TypeError\nfillEmpty(\"ab\")=\"object\"\npush(5)=\"val\"\npop(5)=\"val\"\nshift(5)=\"val\"\nunshift(5)=\"val\"\nreverse(5)=\"obj\"\nsort(5)=\"obj\"\nsplice(5)=\"obj\"\ncopyWithin(5)=\"obj\"\nfill(5)=\"object\"\nfillEmpty(5)=\"object\"\n"],
  [function protochain() {
    const out = []; let log = '';
    function pushes(a, n) { for (let i = 0; i < n; i++) a.push(i); return a; }
    function appends(a, n) { for (let i = 0; i < n; i++) a[a.length] = i; return a; }
    function holeFill(a, n) { for (let i = 0; i < n; i++) a[i] = i; return a; }
    for (let r = 0; r < 3000; r++) { pushes([], 8); appends([], 8); holeFill(new Array(8), 8); }
    const scenarios = [
      ['ArrayProto setter', () => Object.defineProperty(Array.prototype, '3', { set(v) { log += 'AP set ' + v + ';'; }, configurable: true }), () => delete Array.prototype[3]],
      ['ObjectProto setter', () => Object.defineProperty(Object.prototype, '4', { set(v) { log += 'OP set ' + v + ';'; }, configurable: true }), () => delete Object.prototype[4]],
      ['ArrayProto readonly', () => Object.defineProperty(Array.prototype, '2', { value: 'ro', writable: false, configurable: true }), () => delete Array.prototype[2]],
      ['ArrayProto element', () => { Array.prototype[5] = 'elem'; }, () => { Array.prototype.length = 0; }],
      ['proto swap', () => { const p = Object.create(Object.prototype, { 1: { set(v) { log += 'swap set ' + v + ';'; }, configurable: true } }); Object.setPrototypeOf(Array.prototype, p); }, () => Object.setPrototypeOf(Array.prototype, Object.prototype)],
    ];
    for (const [name, install, remove] of scenarios) {
      install();
      const tr = f => { try { return f(); } catch (e) { return e.constructor.name; } };
      const a = tr(() => pushes([], 8)), b = tr(() => appends([], 8)), c = tr(() => holeFill(new Array(8), 8));
      out.push(name + ': ' + JSON.stringify([a, b, c, typeof a === 'string' ? a : Object.keys(a).length, [, 1].join()]) + ' log=' + log); log = '';
      remove();
      out.push(name + ' after: ' + JSON.stringify(pushes([], 4)));
    }
    console.log(out.join('\n'));
  }, "ArrayProto setter: [[0,1,2,null,4,5,6,7],[0,1,2],[0,1,2,null,4,5,6,7],7,\",1\"] log=AP set 3;AP set 3;AP set 4;AP set 5;AP set 6;AP set 7;AP set 3;\nArrayProto setter after: [0,1,2,3]\nObjectProto setter: [[0,1,2,3,null,5,6,7],[0,1,2,3],[0,1,2,3,null,5,6,7],7,\",1\"] log=OP set 4;OP set 4;OP set 5;OP set 6;OP set 7;OP set 4;\nObjectProto setter after: [0,1,2,3]\nArrayProto readonly: [\"TypeError\",[0,1],[0,1,\"ro\",3,4,5,6,7],\"TypeError\",\",1\"] log=\nArrayProto readonly after: [0,1,2,3]\nArrayProto element: [[0,1,2,3,4,5,6,7],[0,1,2,3,4,5,6,7],[0,1,2,3,4,5,6,7],8,\",1\"] log=\nArrayProto element after: [0,1,2,3]\nproto swap: [[0,null,2,3,4,5,6,7],[0],[0,null,2,3,4,5,6,7],7,\",1\"] log=swap set 1;swap set 1;swap set 2;swap set 3;swap set 4;swap set 5;swap set 6;swap set 7;swap set 1;\nproto swap after: [0,1,2,3]\n"],
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
