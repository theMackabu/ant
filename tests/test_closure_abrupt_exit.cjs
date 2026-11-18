const assert = require('node:assert');

const cases = {
  basic() { let f; try { { let x = 5; f = () => x; throw 1; } } catch (e) { return f() } },
  finallyThrow() { let f, r; try { try { { let x = 5; f = () => x; throw 1; } } finally { let y = 9; r = f() } } catch (e) { return [r, f()] } },
  finallyReturn() { let f, r; function g() { try { let x = 5; f = () => x; return 1 } finally { let y = 9; r = f() } } g(); return [r, f()] },
  finallyBreak() { let f, r; for (;;) { try { let x = 5; f = () => x; break } finally { let y = 9; r = f() } } return [r, f()] },
  finallyContinue() { let f, r, n = 0; while (n++ < 1) { try { let x = 5; f = () => x; continue } finally { let y = 9; r = f() } } return [r, f()] },
  nested() { let f, g; try { let a = 1; g = () => a; try { let b = 2; f = () => b; throw 0 } catch (e1) { let c = 7; throw c } } catch (e2) { let d = 8, d2 = 9; return [f(), g(), e2] } },
  loopLet() { const fs = []; try { for (let i = 0; i < 5; i++) { fs.push(() => i); if (i === 2) throw 1 } } catch (e) { let q = 99; return fs.map(f => f()) } },
  loopBody() { const fs = []; try { for (let i = 0; i < 5; i++) { let j = i * 10; fs.push(() => j); if (i === 2) throw 1 } } catch (e) { let q = 99; return fs.map(f => f()) } },
  forOf() { const fs = []; try { for (const v of [1, 2, 3]) { fs.push(() => v); if (v === 2) throw 1 } } catch (e) { let q = 99; return fs.map(f => f()) } },
  forIn() { const fs = []; try { for (const k in { a: 1, b: 2 }) { fs.push(() => k); if (k === 'b') throw 1 } } catch (e) { let q = 99; return fs.map(f => f()) } },
  whileBody() { const fs = []; let n = 0; try { while (true) { let j = n++; fs.push(() => j); if (j === 2) throw 1 } } catch (e) { let q = 99; return fs.map(f => f()) } },
  sw() { let f; try { switch (1) { case 1: let x = 5; f = () => x; throw 1 } } catch (e) { return f() } },
  staticBlock() { let f; class A { static { try { { let x = 5; f = () => x; throw 1 } } catch (e) { let y = 3; A.r = f() } } } return [A.r, f()] },
  gen() { let f; function* g() { try { { let x = 5; f = () => x; yield 1; throw 2 } } catch (e) { let y = 4; yield f() } } const it = g(); it.next(); return [it.next().value, f()] },
  genThrowInto() { let f; function* g() { try { { let x = 5; f = () => x; yield 1 } } catch (e) { let y = 4; yield f() } } const it = g(); it.next(); return [it.throw(7).value, f()] },
  genReturnInto() { let f, r; function* g() { try { { let x = 5; f = () => x; yield 1 } } finally { let y = 4; r = f() } } const it = g(); it.next(); it.return(7); return [r, f()] },
  breakFinally() { let f, r; out: { try { { let x = 5; f = () => x; break out } } finally { let y = 4; r = f() } } return [r, f()] },
  returnFinallyOuterVar() { let f; function g() { let x = 1; f = () => x; try { return 0 } finally { x = 2 } } g(); return f() },
  breakFinallyWrite() { let f; for (;;) { let x = 1; f = () => x; try { break } finally { x = 2 } } return f() },
  continueFinallyWrite() { const fs = []; for (let i = 0; i < 3; i++) { fs.push(() => i); try { continue } finally { i += 10 } } return fs.map(f => f()) },
  continueFinallyWrite2() { const fs = []; let n = 0; while (n++ < 2) { let x = 1; fs.push(() => x); try { continue } finally { x = 2 } } return fs.map(f => f()) },
  destructCatch() { let f; try { { let x = 5; f = () => x; throw { a: 1, b: [2, 3] } } } catch ({ a, b: [c, d] }) { return [f(), a, c, d] } },
  catchless() { let f, r; try { try { { let x = 5; f = () => x; throw 1 } } finally { let y = 6; r = f() } } catch { return [r, f()] } },
  noParam() { let f; try { { let x = 5; f = () => x; throw 1 } } catch { let y = 6; return f() } },
  inCatch() { let f; try { try { throw 1 } catch (e) { let x = 5; f = () => [x, e]; throw 2 } } catch (e2) { let y = 6, z = 7; return f() } },
  inFinally() { let f; try { try { } finally { let x = 5; f = () => x; throw 2 } } catch (e2) { let y = 6; return f() } },
  inCatchFinally() { let f, r; try { try { throw 1 } catch (e) { let x = 5; f = () => [x, e]; throw 2 } finally { let y = 6, z = 7; r = f() } } catch (e3) { return [r, f()] } },
  calleeThrow() { let f; const t = () => { throw 1 }; try { { let x = 5; f = () => x; t() } } catch (e) { return f() } },
  getterThrow() { let f; const o = { get p() { throw 1 } }; try { { let x = 5; f = () => x; o.p } } catch (e) { return f() } },
  tdz() { let f; try { { f = () => x; throw 1; let x = 5 } } catch (e) { try { return f() } catch (e2) { return e2.constructor.name } } },
  deep() { const fs = []; function a(n) { let x = n; fs.push(() => x); if (n === 0) throw 1; { let y = n * 2; fs.push(() => y); a(n - 1) } } try { a(4) } catch (e) { let q = 0; return fs.map(f => f()) } },
  deepTry() { const fs = []; function a(n) { try { let x = n; fs.push(() => x); if (n === 0) throw 1; a(n - 1) } finally { let z = -1; fs.push(() => z) } } try { a(3) } catch (e) { let q = 0; return fs.map(f => f()) } },
  shared() { let f, s; try { { let x = 1; f = () => x; s = v => { x = v }; x = 2; s(3); x++; throw 1 } } catch (e) { const a = f(); s(10); return [a, f()] } },
  sharedBefore() { let r = []; try { { let x = 1; const f = () => x, s = v => { x = v }; s(5); r.push(x); x = 6; r.push(f()); throw 1 } } catch (e) { return r } },
  usingDecl() { let f; const log = []; try { { using d = { [Symbol.dispose]() { log.push(f()) } }; let x = 5; f = () => x; throw 1 } } catch (e) { let y = 6; return [f(), log] } },
  usingDecl2() { let f; const log = []; { using d = { [Symbol.dispose]() { log.push(f()) } }; let x = 5; f = () => x; } let y = 6; return [f(), log] },
  labeled() { let f; a: try { { let x = 5; f = () => x; throw 1 } } catch (e) { break a } let z = 3; return f() },
  argsCapture() { let f; function g(p) { try { { let x = p; f = () => x + p; throw 1 } } catch (e) { let y = 100; return f() } } return g(5) },
  loopTryInside() { const fs = []; for (let i = 0; i < 3; i++) { try { let x = i; fs.push(() => x); throw 1 } catch (e) { let y = 50 + i; fs.push(() => y) } } return fs.map(f => f()) },
  hot() { let s = 0; for (let i = 0; i < 3000; i++) { let f; try { { let x = i; f = () => x; if (i % 3 === 0) throw 1 } } catch (e) { let y = -1; s += f() } } return s },
  hotInl() { function h(i) { let f; try { { let x = i; f = () => x; if (i % 3 === 0) throw 1 } } catch (e) { let y = -1; return f() } return 0 } let s = 0; for (let i = 0; i < 5000; i++) s += h(i); return s },
  hotCallee() { function t(i) { if (i % 2) throw i } function h(i) { let f; try { { let x = i; f = () => x; t(i) } } catch (e) { let y = -1; return f() } return 0 } let s = 0; for (let i = 0; i < 5000; i++) s += h(i); return s },
  evalCap() { let r; try { { let x = 5; r = eval('() => x'); throw 1 } } catch (e) { let y = 9; return r() } },
  classTdz() { let f; try { { f = () => C; throw 1; class C {} } } catch (e) { try { return typeof f() } catch (e2) { return e2.constructor.name } } },
  labeledContinue() { const fs = []; outer: for (let i = 0; i < 3; i++) { for (let j = 0; j < 3; j++) { let x = i * 10 + j; fs.push(() => [x, i, j]); try { if (j === 1) continue outer } finally { x += 100 } } } return fs.map(f => f()) },
  nestedFinallyBreak() { const fs = []; a: { let p = 1; fs.push(() => p); try { let q = 2; fs.push(() => q); try { let r = 3; fs.push(() => r); break a } finally { let s = 40; q += 10; p += 10 } } finally { let t = 50, u = 60; p += 100 } } let z = 7, z2 = 8; return fs.map(f => f()) },
  retNestedFinally() { const fs = []; (function () { let p = 1; fs.push(() => p); try { let q = 2; fs.push(() => q); try { let r = 3; fs.push(() => r); return } finally { let s = 40; q += 10; p += 10 } } finally { let t = 50, u = 60; p += 100 } })(); return fs.map(f => f()) },
  withStmt: new Function("let f; try { with ({ k: 1 }) { let x = 5; f = () => x + k; throw 1 } } catch (e) { let y = 9; return f() }"),
  derivedCtor() { let f, r; class P { } class D extends P { constructor() { super(); try { let x = 5; f = () => x; return } finally { let y = 9; r = f() } } } new D(); return [r, f()] },
  forUsing() { const log = []; let f; try { for (using d of [{ [Symbol.dispose]() { log.push(f()) } }]) { let x = 5; f = () => x; throw 1 } } catch (e) { let y = 0; return [f(), log] } },
  genLoop() { const fs = []; function* g() { for (let i = 0; i < 3; i++) { try { let x = i; fs.push(() => x); yield i } finally { let y = -1; fs.push(() => y) } } } const it = g(); it.next(); it.next(); it.return(); let pad = 3; return fs.map(f => f()) },
  rethrow() { let f; try { try { { let x = 5; f = () => x; throw 1 } } catch (e) { let a = 6; throw e } } catch (e) { let b = 7, c = 8; return f() } },
  catchParamCapture() { let f; try { try { throw 5 } catch (e) { f = () => e; throw 6 } } catch (e2) { let q = 9; return [f(), e2] } },
  paramDefaults() { let f; function g(a = (() => { try { { let x = 5; f = () => x; throw 1 } } catch (e) { return 2 } })()) { let b = 3; return [a, f()] } return g() },
  switchLoop() { const fs = []; for (let i = 0; i < 3; i++) { try { switch (i) { case 1: { let x = 'one'; fs.push(() => x); throw 0 } default: { let x = 'd' + i; fs.push(() => x) } } } catch (e) { let y = 'c'; fs.push(() => y) } } return fs.map(f => f()) },
  tryInArrow() { let f; const g = () => { try { { let x = 5; f = () => x; throw 1 } } catch { let y = 6; return f() } }; return g() },
  osrLoop() { let s = 0; let f; for (let i = 0; i < 20000; i++) { try { { let x = i; if (i % 1000 === 0) { f = () => x; throw 1 } s += x & 1 } } catch (e) { let y = -5; s += f() } } return s },
  fnDecl() { let f; try { { let x = 5; function inner() { return x } f = inner; throw 1 } } catch (e) { let y = 1; return f() } },
};
const asyncCases = {
  async aw() { let f; try { { let x = 5; f = () => x; await null; throw 1 } } catch (e) { let y = 3; return f() } },
  async awReject() { let f; try { { let x = 5; f = () => x; await Promise.reject(1) } } catch (e) { let y = 3; return f() } },
  async awFinally() { let f, r; try { try { { let x = 5; f = () => x; await Promise.reject(1) } } finally { let y = 3; await null; r = f() } } catch (e) { return [r, f()] } },
  async asyncGen() { let f; async function* g() { try { { let x = 5; f = () => x; yield 1; await null; throw 2 } } catch (e) { let y = 4; yield f() } } const it = g(); await it.next(); return [(await it.next()).value, f()] },
  async forAwait() { const fs = []; try { for await (const v of [1, 2, 3]) { fs.push(() => v); if (v === 2) throw 1 } } catch (e) { let q = 99; return fs.map(f => f()) } },
  async awaitInFinally() { const fs = []; async function g() { let p = 1; fs.push(() => p); try { let q = 2; fs.push(() => q); return 0 } finally { let s = 40; await null; p += 10 } } await g(); return fs.map(f => f()) },
  async asyncGenReturn() { let f, r; async function* g() { try { { let x = 5; f = () => x; yield 1 } } finally { let y = 4; await null; r = f() } } const it = g(); await it.next(); await it.return(7); return [r, f()] },
  async rejectDeep() { const fs = []; async function a(n) { let x = n; fs.push(() => x); await null; if (n === 0) throw 1; try { let y = n * 2; fs.push(() => y); await a(n - 1) } finally { let z = -n; fs.push(() => z) } } try { await a(2) } catch (e) { let q = 0 } return fs.map(f => f()) },
  async awaitUsing() { let f; const log = []; try { { await using d = { async [Symbol.asyncDispose]() { log.push(f()) } }; let x = 5; f = () => x; throw 1 } } catch (e) { let y = 6; return [f(), log] } },
};

const D = (log, n) => ({ [Symbol.dispose]() { log.push(n) } });
Object.assign(cases, {
  retOrder() { const log = []; (function () { using d = D(log, 'd'); try { return 1 } finally { log.push('f') } })(); return log },
  brkOrder() { const log = []; for (;;) { using d = D(log, 'd'); try { break } finally { log.push('f') } } return log },
  contOrder() { const log = []; let n = 0; while (n++ < 1) { using d = D(log, 'd'); try { continue } finally { log.push('f') } } return log },
  brkInner() { const log = []; for (;;) { try { using d = D(log, 'd'); break } finally { log.push('f') } } return log },
  brkBoth() { const log = []; for (;;) { using a = D(log, 'a'); try { using d = D(log, 'd'); try { using e = D(log, 'e'); break } finally { log.push('f1') } } finally { log.push('f2') } } return log },
  retBoth() { const log = []; (function () { using a = D(log, 'a'); try { using d = D(log, 'd'); try { using e = D(log, 'e'); return 1 } finally { log.push('f1') } } finally { log.push('f2') } })(); return log },
  forOfBrk() { const log = []; const it = { [Symbol.iterator]() { return { next() { return { value: 1, done: false } }, return() { log.push('ret'); return {} } } } }; for (const v of it) { using a = D(log, 'a'); try { using d = D(log, 'd'); break } finally { log.push('f') } } return log },
  forOfRet() { const log = []; const it = { [Symbol.iterator]() { return { next() { return { value: 1, done: false } }, return() { log.push('ret'); return {} } } } }; (function () { for (const v of it) { using a = D(log, 'a'); try { using d = D(log, 'd'); return 1 } finally { log.push('f') } } })(); return log },
  forOfClosure() { const fs = []; const it = { [Symbol.iterator]() { return { next() { return { value: 1, done: false } }, return() { fs.push(fs[0]()); return {} } } } }; for (const v of it) { let x = 1; fs.push(() => x); try { break } finally { x = 2 } } fs.push(fs[0]()); return fs.slice(1) },
});

const expected = {
  "basic": 5,
  "finallyThrow": [5, 5],
  "finallyReturn": [5, 5],
  "finallyBreak": [5, 5],
  "finallyContinue": [5, 5],
  "nested": [2, 1, 7],
  "loopLet": [0, 1, 2],
  "loopBody": [0, 10, 20],
  "forOf": [1, 2],
  "forIn": ["a", "b"],
  "whileBody": [0, 1, 2],
  "sw": 5,
  "staticBlock": [5, 5],
  "gen": [5, 5],
  "genThrowInto": [5, 5],
  "genReturnInto": [5, 5],
  "breakFinally": [5, 5],
  "returnFinallyOuterVar": 2,
  "breakFinallyWrite": 2,
  "continueFinallyWrite": [10],
  "continueFinallyWrite2": [2, 2],
  "destructCatch": [5, 1, 2, 3],
  "catchless": [5, 5],
  "noParam": 5,
  "inCatch": [5, 1],
  "inFinally": 5,
  "inCatchFinally": [[5, 1], [5, 1]],
  "calleeThrow": 5,
  "getterThrow": 5,
  "tdz": "ReferenceError",
  "deep": [4, 8, 3, 6, 2, 4, 1, 2, 0],
  "deepTry": [3, 2, 1, 0, -1, -1, -1, -1],
  "shared": [4, 10],
  "sharedBefore": [5, 6],
  "usingDecl": [5, [5]],
  "usingDecl2": [5, [5]],
  "labeled": 5,
  "argsCapture": 10,
  "loopTryInside": [0, 50, 1, 51, 2, 52],
  "hot": 1498500,
  "hotInl": 4165833,
  "hotCallee": 6250000,
  "evalCap": 5,
  "classTdz": "ReferenceError",
  "labeledContinue": [[100, 0, 0], [101, 0, 1], [110, 1, 0], [111, 1, 1], [120, 2, 0], [121, 2, 1]],
  "nestedFinallyBreak": [111, 12, 3],
  "retNestedFinally": [111, 12, 3],
  "withStmt": 6,
  "derivedCtor": [5, 5],
  "forUsing": [5, [5]],
  "genLoop": [0, -1, 1, -1],
  "rethrow": 5,
  "catchParamCapture": [5, 6],
  "paramDefaults": [2, 5],
  "switchLoop": ["d0", "one", "c", "d2"],
  "tryInArrow": 5,
  "osrLoop": 200000,
  "fnDecl": 5,
  "retOrder": ["f", "d"],
  "brkOrder": ["f", "d"],
  "contOrder": ["f", "d"],
  "brkInner": ["d", "f"],
  "brkBoth": ["e", "f1", "d", "f2", "a"],
  "retBoth": ["e", "f1", "d", "f2", "a"],
  "forOfBrk": ["d", "f", "a", "ret"],
  "forOfRet": ["d", "f", "a", "ret"],
  "forOfClosure": [2, 2],
  "aw": 5,
  "awReject": 5,
  "awFinally": [5, 5],
  "asyncGen": [5, 5],
  "forAwait": [1, 2],
  "awaitInFinally": [11, 2],
  "asyncGenReturn": [5, 5],
  "rejectDeep": [2, 4, 1, 2, 0, -1, -2],
  "awaitUsing": [5, [5]],
};

const hotCases = {
  getterThrowSeesWrite: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; g(); const o = { get x() { g(); if (k) throw 1; return 2; } }; o.x; return 'N' + h() + v; } catch (e) { return 'C' + h(); } },
  getterThrowFinally: (k) => { let g, h, r = ''; try { try { let v = 10; g = () => v += 1; h = () => v; const o = { get x() { g(); if (k) throw 1; return 2; } }; o.x; r += 'N' + v; } finally { r += 'F' + h(); } } catch (e) { r += 'C' + h(); } return r; },
  forOfNextThrow: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; const it = { [Symbol.iterator]() { return { next() { g(); if (k) throw 1; return { done: true }; } }; } }; for (const x of it) void x; return 'N' + h() + v; } catch (e) { return 'C' + h(); } },
  forOfReturnThrow: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; const it = { [Symbol.iterator]() { return { next() { return { done: false, value: 1 }; }, return() { g(); if (k) throw 1; return {}; } }; } }; for (const x of it) { void x; break; } return 'N' + h() + v; } catch (e) { return 'C' + h(); } },
  destructureThrow: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; const it = { [Symbol.iterator]() { return { next() { g(); if (k) throw 1; return { done: true }; } }; } }; const [a] = it; return 'N' + h() + v + a; } catch (e) { return 'C' + h(); } },
  spreadThrow: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; const it = { [Symbol.iterator]() { return { next() { g(); if (k) throw 1; return { done: true }; } }; } }; const a = [...it]; return 'N' + h() + v + a.length; } catch (e) { return 'C' + h(); } },
  forInProxyThrow: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; const p = new Proxy({}, { ownKeys() { g(); if (k) throw 1; return []; } }); for (const x in p) void x; return 'N' + h() + v; } catch (e) { return 'C' + h(); } },
  setterThrow: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; const o = { set x(_) { g(); if (k) throw 1; } }; o.x = 1; return 'N' + h() + v; } catch (e) { return 'C' + h(); } },
  toPrimitiveThrow: (k) => { let g, h; try { let v = 10; g = () => v += 1; h = () => v; const o = { valueOf() { g(); if (k) throw 1; return 1; } }; const n = o + 1; return 'N' + h() + v + n; } catch (e) { return 'C' + h(); } },
};

const hotExpected = {
  getterThrowSeesWrite: ["N1212", "C12"],
  getterThrowFinally: ["N11F11", "F11C11"],
  forOfNextThrow: ["N1111", "C11"],
  forOfReturnThrow: ["N1111", "C11"],
  destructureThrow: ["N1111undefined", "C11"],
  spreadThrow: ["N11110", "C11"],
  forInProxyThrow: ["N1111", "C11"],
  setterThrow: ["N1111", "C11"],
  toPrimitiveThrow: ["N11112", "C11"],
};

for (const [name, run] of Object.entries(hotCases)) {
  for (let i = 0; i < 600; i++) {
    for (let k = 0; k < 2; k++) assert.strictEqual(run(k), hotExpected[name][k], `${name} k=${k} iteration ${i}`);
  }
}
assert.strictEqual(Object.keys(hotExpected).length, Object.keys(hotCases).length);

const finallyCloseCases = {
  returnThroughFinally(k) {
    const fs = [], out = [];
    const r = (function () {
      try {
        let v = 2;
        fs.push(() => v++);
        if (k) return 'r';
      } finally {
        let reused = 'f';
        out.push(fs[0](), reused);
      }
      return 'e';
    })();
    return r + out.join() + fs[0]();
  },
  breakThroughFinally(k) {
    const fs = [], out = [];
    for (let i = 0; i < 3; i++) {
      try {
        let v = i * 10;
        fs.push(() => v += 1);
        if (k && i === 1) break;
        if (i === 0) continue;
      } finally {
        let reused = 'f' + i;
        out.push(fs[fs.length - 1](), reused);
      }
    }
    return out.join() + '|' + fs.map(f => f()).join();
  },
  nestedReturnThroughFinally(k) {
    const fs = [], out = [];
    const r = (function () {
      try {
        let a = 1;
        fs.push(() => a += 1);
        try {
          let b = 10;
          fs.push(() => b += 10);
          if (k) return 'r';
        } finally {
          let inner = 'i';
          out.push(fs[1](), inner);
        }
      } finally {
        let outer = 'o';
        out.push(fs[0](), outer);
      }
      return 'e';
    })();
    return r + out.join() + '|' + fs.map(f => f()).join();
  },
};
const finallyCloseExpected = {
  returnThroughFinally: ['e2,f3', 'r2,f3'],
  breakThroughFinally: ['1,f0,11,f1,21,f2|2,12,22', '1,f0,11,f1|2,12'],
  nestedReturnThroughFinally: ['e20,i,2,o|3,30', 'r20,i,2,o|3,30'],
};
for (const [name, run] of Object.entries(finallyCloseCases)) {
  for (let i = 0; i < 600; i++) {
    for (let k = 0; k < 2; k++) assert.strictEqual(run(k), finallyCloseExpected[name][k], `${name} k=${k} iteration ${i}`);
  }
}

function show(v) {
  if (typeof v === 'function') return 'function ' + v.name;
  if (typeof v === 'symbol' || typeof v === 'bigint') return String(v);
  if (v === undefined) return 'undefined';
  try { return JSON.stringify(v) } catch (e) { return 'unserializable' }
}

function outcome(src) {
  let fn;
  try { fn = new Function(src) } catch (e) { return 'compile ' + e.constructor.name }
  try { return 'value ' + show(fn()) } catch (e) { return 'throw ' + (e && e.constructor ? e.constructor.name : String(e)) }
}

const sourceCases = [
  ["var f; function g() { try { (class C extends (f = () => C, null.x) {}) } finally { let a = 'leak'; return f() } } return g()", "throw ReferenceError"],
  ["var f; function g() { try { (class C { [(f = () => C, null.x)]() {} }) } finally { let a = 'leak'; return f() } } return g()", "throw ReferenceError"],
  ["var f; function g() { try { (class C { static [(f = () => C, null.x)] = 1 }) } finally { let a = 'leak'; return f() } } return g()", "throw ReferenceError"],
  ["var f; function g() { try { (class C { static x = (f = () => C, null.x) }) } catch (e) { let a = 'leak'; return f().name } } return g()", "value \"C\""],
  ["var f; function g() { try { (class C { static { f = () => C; null.x } }) } catch (e) { let a = 'leak'; return f().name } } return g()", "value \"C\""],
  ["var f; function g() { try { class C extends (f = () => C, null.x) {} } catch (e) { let a = 'leak', b = 'leak2'; return f() } } return g()", "throw ReferenceError"],
  ["var f; function g() { try { class C { [(f = () => C, null.x)] = 1 } } catch (e) { let a = 'leak', b = 'leak2'; return f() } } return g()", "throw ReferenceError"],
  ["var f; try { { let x = 1; f = () => x; null.x } } catch (e) {} return f()", "value 1"],
];

const sourceFailed = [];
for (const [src, want] of sourceCases) {
  const got = outcome(src);
  if (got !== want) sourceFailed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(sourceFailed, []);

(async () => {
  const failed = [];
  const check = (k, v) => {
    const got = JSON.stringify(v);
    const want = JSON.stringify(expected[k]);
    if (got !== want) failed.push(`${k}: got ${got}, want ${want}`);
  };
  const run = async (f) => { try { return await f() } catch (e) { return 'THROW ' + e.constructor.name } };
  for (const k in cases) for (let i = 0; i < 3; i++) check(k, await run(cases[k]));
  for (const k in asyncCases) check(k, await run(asyncCases[k]));
  assert.deepStrictEqual(failed, []);
  assert.strictEqual(Object.keys(expected).length, Object.keys(cases).length + Object.keys(asyncCases).length);
  console.log('ok');
})();
