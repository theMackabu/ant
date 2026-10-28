const assert = require('node:assert');

const warm = 600;
const out = [];
function show(name, value) {
  out.push(`${name}: ${JSON.stringify(value)}`);
}

function frames(e) {
  return String(e.stack).split('\n').slice(0, 5).map(line => line.trim().replace(/\(.*[\\/]/, '('));
}

function thrower(i) {
  if (i % 100 === 0) throw new Error(`boom ${i}`);
  return i;
}

function catchEvery(n) {
  let s = 0, c = 0;
  for (let i = 0; i < n; i++) {
    try {
      s += thrower(i);
    } catch (e) {
      c++;
      s -= e.message.length;
    }
  }
  return [s, c];
}

function lateCatch(n, at) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try {
      if (i === at) throw i;
      s += i;
    } catch (e) {
      s -= e * 2;
    }
  }
  return s;
}

function nested(i) {
  let log = '';
  try {
    try {
      if (i & 1) throw i;
      log += 'a';
    } catch (e) {
      log += 'b' + e;
      if (i & 2) throw 'x' + e;
    }
    log += 'c';
  } catch (e2) {
    log += 'd' + e2;
  }
  return log;
}

function noBinding(i) {
  try {
    if (i % 7 === 0) throw 1;
    return 1;
  } catch {
    return 2;
  }
}

function rethrow(i) {
  try {
    try {
      if (i % 5 === 0) throw new TypeError('t' + i);
      return 0;
    } catch (e) {
      if (e instanceof TypeError) throw e;
      return 1;
    }
  } catch (e) {
    return e.message;
  }
}

function unboxedLive(n) {
  let a = 1, b = 2.5, d = true, k = 0;
  for (let i = 0; i < n; i++) {
    a = (a + i) | 0;
    b = b * 1.0001;
    d = !d;
    try {
      if (i % 13 === 0) throw i;
      k += a & 7;
    } catch (e) {
      a -= e;
      k += d ? 1 : 2;
    }
  }
  return [a, b.toFixed(6), d, k];
}

function capturedInTry(n) {
  let x = 0;
  const read = () => x;
  for (let i = 0; i < n; i++) {
    try {
      x = i;
      if (i % 11 === 0) throw read();
    } catch (e) {
      x = e * 2 + read();
    }
  }
  return [x, read()];
}

function argumentsInTry() {
  'use strict';
  let s = 0;
  for (let i = 0; i < arguments.length * 50; i++) {
    try {
      if (i % 9 === 0) throw arguments[i % arguments.length];
      s += arguments[0];
    } catch (e) {
      s += e * 10;
    }
  }
  return s;
}

function fewArgs(a, b, c) {
  try {
    if (c === undefined) throw a;
    return a + b + c;
  } catch (e) {
    return [e, b];
  }
}

function getterThrows(o) {
  try {
    return o.v;
  } catch (e) {
    return 'caught ' + e;
  }
}

function valueOfThrows(o, n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try {
      s += i + (i % 50 === 0 ? o : 1);
    } catch (e) {
      s += 1000;
    }
  }
  return s;
}

function nativeThrows(texts) {
  let ok = 0, bad = 0;
  for (const t of texts) {
    try {
      ok += JSON.parse(t).v;
    } catch (e) {
      bad += e instanceof SyntaxError ? 1 : 100;
    }
  }
  return [ok, bad];
}

function inner(i) {
  if (i % 9 === 0) throw new RangeError('r' + i);
  return i;
}
function middle(i) {
  return inner(i) + 1;
}
function outer(i) {
  try {
    return middle(i);
  } catch (e) {
    return e.name + e.message;
  }
}

function stackOf(i) {
  try {
    middle(i * 9);
  } catch (e) {
    return frames(e);
  }
  return null;
}

function deep(n) {
  return n === 0 ? 0 : 1 + deep(n - 1);
}
function overflowCaught() {
  try {
    return deep(1e7);
  } catch (e) {
    return e instanceof RangeError;
  }
}

function sumOf(a) {
  let s = 0;
  for (const x of a) s += x;
  return s;
}

function pairs(p) {
  const [a, b] = p;
  return a * 10 + b;
}

function holesAndDefaults(p) {
  const [, b = 7, c = b + 1, ...rest] = p;
  return [b, c, rest.length];
}

function throwingDefault(p) {
  try {
    const [a = (() => { throw new Error('default'); })()] = p;
    return a;
  } catch (e) {
    return e.message;
  }
}

function notIterable(v) {
  try {
    const [a] = v;
    return a;
  } catch (e) {
    return e.constructor.name;
  }
}

function* gen(n) {
  for (let i = 0; i < n; i++) yield i;
}
function sumGen(n) {
  let s = 0;
  for (const x of gen(n)) s += x;
  return s;
}
function* failingGen(n) {
  yield 1;
  if (n > 2) throw new Error('gen ' + n);
  yield 2;
}
function catchGen(n) {
  let s = 0;
  try {
    for (const x of failingGen(n)) s += x;
  } catch (e) {
    s += e.message.length * 100;
  }
  return s;
}
function restFromGen(n) {
  const [first, ...rest] = gen(n);
  return [first, rest.length];
}

function mapEntries(m) {
  let s = 0;
  for (const [k, v] of m) s += k * v;
  return s;
}
function setValues(st) {
  let s = 0;
  for (const v of st) s += v;
  return s;
}
function reversed(str) {
  let s = '';
  for (const ch of str) s = ch + s;
  return s;
}
function mixedIterables(list) {
  let s = 0;
  for (const it of list) for (const v of it) s += typeof v === 'number' ? v : v.length;
  return s;
}

let closes = 0;
function countedIterable(limit, failNextAt, failReturn) {
  return {
    [Symbol.iterator]() {
      let i = 0;
      return {
        next() {
          i++;
          if (i === failNextAt) throw new Error('next ' + i);
          return { value: i, done: i > limit };
        },
        return() {
          closes++;
          if (failReturn) throw new Error('return');
          return {};
        },
      };
    },
  };
}
function breakEarly(it, n) {
  let s = 0;
  for (const x of it) {
    if (x > n) break;
    s += x;
  }
  return s;
}
function throwInside(it, n) {
  let s = 0;
  try {
    for (const x of it) {
      if (x === n) throw new Error('in ' + x);
      s += x;
    }
  } catch (e) {
    s += 100;
  }
  return s;
}
function returnInside(it, n) {
  for (const x of it) if (x === n) return x;
  return -1;
}
function labeled(a, it) {
  let s = 0;
  outerLoop: for (const x of a) {
    for (const y of it) {
      if (y === 2) continue outerLoop;
      if (x === 3) break outerLoop;
      s += x * y;
    }
  }
  return s;
}
function catchReturnThrow(it) {
  try {
    for (const x of it) if (x === 2) break;
    return 'no';
  } catch (e) {
    return e.message;
  }
}

function mutateDuring(n) {
  const a = [1, 2, 3];
  let s = 0, steps = 0;
  for (const x of a) {
    s += x;
    if (a.length < n) a.push(x + 1);
    if (++steps > 100) break;
  }
  return [s, a.length];
}
function holey(a) {
  let s = 0, u = 0;
  for (const x of a) {
    if (x === undefined) u++;
    else s += x;
  }
  return [s, u];
}
function typedValues(t) {
  let s = 0;
  for (const x of t) s += x;
  return s;
}
function argumentsIteration() {
  'use strict';
  let s = 0;
  for (const x of arguments) s += x;
  return s;
}
class Bag extends Array {}
function subclassIteration(b) {
  let s = 0;
  for (const x of b) s += x;
  return s;
}

function osrInTry(n) {
  let s = 0;
  try {
    for (let i = 0; i < n; i++) {
      s += i;
      if (i === n - 1) throw s;
    }
  } catch (e) {
    return e + 1;
  }
  return s;
}

function catchThenLoop(n) {
  let s = 0;
  for (let r = 0; r < n; r++) {
    try {
      if (r % 4 === 0) throw r;
      s += r;
    } catch (e) {
      for (let k = 0; k < 3; k++) s += e + k;
    }
  }
  return s;
}

function patchedIterator(a) {
  let s = 0;
  for (const x of a) s += x;
  return s;
}

let finallyRuns = 0;
function finallyNormal(i) {
  let s = i;
  try {
    s += 1;
  } finally {
    finallyRuns++;
    s *= 2;
  }
  return s;
}
function returnThroughFinally(i) {
  try {
    if (i & 1) return 'odd';
  } finally {
    finallyRuns++;
  }
  return 'even';
}
function finallyOverridesReturn(i) {
  try {
    return 'try';
  } finally {
    if (i % 3 === 0) return 'finally';
  }
}
function throwThroughFinally(i) {
  try {
    try {
      if (i % 5 === 0) throw new Error('t' + i);
      return 1;
    } finally {
      finallyRuns++;
    }
  } catch (e) {
    return e.message;
  }
}
function finallySwallows(i) {
  try {
    throw i;
  } finally {
    return 'swallowed';
  }
}
function breakContinueFinally(n) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try {
      if (i % 4 === 0) continue;
      if (i === n - 3) break;
      s += i;
    } finally {
      finallyRuns++;
    }
  }
  return s;
}
function nestedFinally(i) {
  let log = '';
  try {
    try {
      log += 'a';
      if (i & 1) throw 'x';
    } finally {
      log += 'b';
    }
  } catch (e) {
    log += 'c' + e;
  } finally {
    log += 'd';
  }
  return log;
}
function catchAndFinally(i) {
  let log = '';
  try {
    if (i % 3 === 0) throw 'e';
    log += 't';
  } catch (e) {
    log += 'c';
  } finally {
    log += 'f';
  }
  return log;
}
function loopInFinally(n) {
  let s = 0;
  try {
    for (let i = 0; i < n; i++) s += i;
  } finally {
    for (let k = 0; k < 3; k++) s += k;
  }
  return s;
}
function loopInFinallyAfterThrow(n) {
  try {
    try {
      throw new Error('inner');
    } finally {
      for (let k = 0; k < n; k++) finallyRuns++;
    }
  } catch (e) {
    return e.message + finallyRuns;
  }
  return null;
}
function forOfInFinally(it, n) {
  let s = 0;
  try {
    for (const x of it) {
      if (x === n) break;
      s += x;
    }
  } finally {
    s += 100;
  }
  return s;
}
function returnFromForOfThroughFinally(it, n) {
  try {
    for (const x of it) if (x === n) return x;
  } finally {
    finallyRuns++;
  }
  return -1;
}
function labeledThroughFinally(n) {
  let s = 0;
  outerLoop: for (let i = 0; i < n; i++) {
    for (let j = 0; j < 5; j++) {
      try {
        if (j === 3) continue outerLoop;
        if (i === n - 1) break outerLoop;
        s += j;
      } finally {
        s += 1000;
      }
    }
  }
  return s;
}
function throwFromFinally(i) {
  try {
    try {
      return 1;
    } finally {
      if (i % 4 === 0) throw new Error('fin' + i);
    }
  } catch (e) {
    return e.message;
  }
}
function exitInFinally(i, o) {
  let s = 0;
  try {
    s = i;
  } finally {
    s += o.v;
  }
  return s;
}

function pendingReturnExit(i, o) {
  try {
    return i * 2;
  } finally {
    finallyRuns += o.v;
  }
}
function pendingJumpExit(n, o) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try {
      if (i % 3 === 0) continue;
      if (i === n - 2) break;
      s += i;
    } finally {
      s += o.v;
    }
  }
  return s;
}
function nestedPendingReturn(i, o) {
  let log = '';
  try {
    try {
      return 'r' + i;
    } finally {
      log += o.a;
      finallyRuns += log.length;
    }
  } finally {
    finallyRuns += o.b;
  }
}
function pendingReturnCatchInFinally(i, o) {
  try {
    return i;
  } finally {
    try {
      if (o.t) throw new Error('inner');
      finallyRuns++;
    } catch (e) {
      finallyRuns += 10;
    }
  }
}
function helperInFinally(o) {
  return o.v + 1;
}
function pendingReturnInlinedExit(i, o) {
  try {
    return i;
  } finally {
    finallyRuns += helperInFinally(o);
  }
}
function returnInFinallyOverridesJump(n) {
  for (let i = 0; i < n; i++) {
    try {
      if (i === 5) break;
      if (i % 2) continue;
    } finally {
      if (i === n - 1) return 'f' + i;
    }
  }
  return 'done';
}
function breakOutOfFinally(i) {
  let r = 'none';
  for (let k = 0; k < 2; k++) {
    try {
      return 'try' + i;
    } finally {
      r = 'fin' + k;
      break;
    }
  }
  return r;
}

class OptOnly {
  constructor(base) {
    this.base = base;
  }
  scaled(list, stop) {
    let s = 0;
    try {
      for (const x of list) {
        if (x === stop) throw new RangeError('stop' + x);
        s += x ** 2;
      }
    } catch (e) {
      s = -s - this.base;
    } finally {
      s += 1;
    }
    return s;
  }
}

function boxedOutside(n) {
  let total = 0;
  const get = () => total;
  for (let i = 0; i < n; i++) {
    try {
      if (i % 7 === 0) throw i;
      total += 1;
    } catch (e) {
      total += 2;
    }
  }
  return [total, get()];
}
function capturedInCatch(n) {
  const fns = [];
  let k = 0;
  for (let i = 0; i < n; i++) {
    try {
      if (i % 5 === 0) throw i;
      k++;
    } catch (e) {
      fns.push(() => e + k);
    }
  }
  return fns.reduce((a, f) => a + f(), 0);
}
function unrelatedClosure(n) {
  let a = 0;
  const b = 1;
  const f = () => b;
  for (let i = 0; i < n; i++) {
    try {
      if (i % 3 === 0) throw i;
      a += i;
    } catch (e) {
      a -= 1;
    }
  }
  return a + f();
}

async function asyncTry(i) {
  try {
    if (i % 3 === 0) throw new Error('async ' + i);
    return i;
  } catch (e) {
    return e.message;
  }
}

function exhausted(it) {
  const [a, b, c] = it;
  return (a === undefined ? 0 : a) + (b === undefined ? 0 : b) + (c === undefined ? 0 : c);
}

function nextThrowsDestructure(it) {
  try {
    const [a, b] = it;
    return a + b;
  } catch (e) {
    return -1;
  }
}

let arrayReturns = 0;
function arrayHead(p) {
  const [a] = p;
  return a;
}

function arrayExact(p) {
  const [a, b] = p;
  return a + b;
}

function arrayShort(p) {
  const [a, b, c] = p;
  return a + b + (c === undefined ? 0 : c);
}

function arrayBreak(p) {
  let s = 0;
  for (const x of p) {
    if (x > 1) break;
    s += x;
  }
  return s;
}

function arrayCases() {
  let r = 0;
  for (let i = 0; i < warm * 2; i++)
    r += arrayHead([i, 1]) + arrayExact([i, 2]) + arrayShort(i % 2 ? [i, 3] : [i, 3, 4]) + arrayBreak([1, i % 3, 5]);
  return r;
}

function runCases() {
  show('arraysBeforeReturn', [arrayCases(), arrayReturns]);
  show('catchEvery', catchEvery(5000));
  let late = 0;
  for (let k = 0; k < warm; k++) late += lateCatch(30, 1000);
  for (let k = 0; k < 40; k++) late += lateCatch(30, k % 30);
  show('lateCatch', late);
  const nl = [];
  for (let i = 0; i < warm; i++) nl.push(nested(i));
  show('nested', nl.slice(-8));
  let nb = 0;
  for (let i = 0; i < warm; i++) nb += noBinding(i);
  show('noBinding', nb);
  const rt = [];
  for (let i = 0; i < warm; i++) rt.push(rethrow(i));
  show('rethrow', rt.slice(-10));
  show('unboxedLive', unboxedLive(3000));
  show('capturedInTry', capturedInTry(3000));
  let ai = 0;
  for (let k = 0; k < warm; k++) ai += argumentsInTry(1, 2, 3);
  show('argumentsInTry', ai);
  const fa = [];
  for (let k = 0; k < warm; k++) fa.push(fewArgs(k, 2, k % 4 ? 3 : undefined));
  show('fewArgs', fa.slice(-4));
  let gcount = 0;
  const go = { get v() { if (gcount++ % 50 === 0) throw 'g' + gcount; return 1; } };
  const gt = [];
  for (let k = 0; k < warm * 2; k++) gt.push(getterThrows(go));
  show('getterThrows', gt.filter(x => x !== 1));
  show('valueOfThrows', valueOfThrows({ valueOf() { throw new Error('v'); } }, 3000));
  const texts = [];
  for (let k = 0; k < 2000; k++) texts.push(k % 17 ? `{"v":${k}}` : '{bad');
  show('nativeThrows', nativeThrows(texts));
  const oo = [];
  for (let i = 0; i < warm * 2; i++) oo.push(outer(i));
  show('outer', oo.slice(-10));
  let sk = null;
  for (let i = 0; i < warm; i++) sk = stackOf(i);
  show('stackOf', sk);
  show('overflowCaught', overflowCaught());

  let so = 0;
  for (let i = 0; i < warm * 3; i++) so += sumOf([i, 1, 2, 3]);
  so += sumOf([1.5, 2.5]) + sumOf(new Set([4, 5])) + sumOf('12'.split('').map(Number));
  show('sumOf', so);
  let pp = 0;
  for (let i = 0; i < warm * 3; i++) pp += pairs([i, i + 1]);
  pp += pairs(new Set([3, 4])) + pairs('56');
  show('pairs', pp);
  const hd = [];
  for (let i = 0; i < warm; i++) hd.push(holesAndDefaults(i % 3 ? [i, undefined, i, i] : [i]));
  show('holesAndDefaults', hd.slice(-3));
  const td = [];
  for (let i = 0; i < warm; i++) td.push(throwingDefault(i % 5 ? [i] : []));
  show('throwingDefault', td.slice(-5));
  const ni = [];
  for (let i = 0; i < warm; i++) ni.push(notIterable(i % 4 ? [i] : i));
  show('notIterable', ni.slice(-4));
  let sg = 0;
  for (let i = 0; i < warm; i++) sg += sumGen(i % 20);
  show('sumGen', sg);
  let cg = 0;
  for (let i = 0; i < warm; i++) cg += catchGen(i % 5);
  show('catchGen', cg);
  const rg = [];
  for (let i = 0; i < warm; i++) rg.push(restFromGen(i % 6));
  show('restFromGen', rg.slice(-6));
  const mm = new Map([[1, 2], [3, 4], [5, 6]]);
  let me = 0;
  for (let i = 0; i < warm * 2; i++) me += mapEntries(mm);
  show('mapEntries', me);
  const st = new Set([1, 2, 3, 4]);
  let sv = 0;
  for (let i = 0; i < warm * 2; i++) sv += setValues(st);
  show('setValues', sv);
  let rv = '';
  for (let i = 0; i < warm; i++) rv = reversed('héllo 😀' + (i % 10));
  show('reversed', rv);
  let mix = 0;
  for (let i = 0; i < warm; i++) mix += mixedIterables([[1, 2], new Set([3]), 'ab', new Map([[1, 1]]).keys(), gen(3)]);
  show('mixedIterables', mix);

  closes = 0;
  let be = 0;
  for (let i = 0; i < warm * 2; i++) be += breakEarly(countedIterable(5, 0, false), i % 6);
  show('breakEarly', [be, closes]);
  closes = 0;
  let ti = 0;
  for (let i = 0; i < warm * 2; i++) ti += throwInside(countedIterable(5, 0, false), i % 7);
  show('throwInside', [ti, closes]);
  closes = 0;
  let ri = 0;
  for (let i = 0; i < warm * 2; i++) ri += returnInside(countedIterable(5, 0, false), i % 7);
  show('returnInside', [ri, closes]);
  closes = 0;
  let nt = 0;
  for (let i = 0; i < warm; i++) nt += throwInside(countedIterable(5, 3, false), 10);
  show('nextThrows', [nt, closes]);
  closes = 0;
  const crt = [];
  for (let i = 0; i < warm; i++) crt.push(catchReturnThrow(countedIterable(5, 0, i % 2 === 0)));
  show('catchReturnThrow', [crt.slice(-2), closes]);
  closes = 0;
  let lb = 0;
  for (let i = 0; i < warm; i++) lb += labeled([1, 2, 3, 4], countedIterable(3, 0, false));
  show('labeled', [lb, closes]);

  show('mutateDuring', [mutateDuring(10), mutateDuring(1000)]);
  let ho = [0, 0];
  for (let i = 0; i < warm; i++) {
    const r = holey([1, , 3, undefined, 5]);
    ho = [ho[0] + r[0], ho[1] + r[1]];
  }
  show('holey', ho);
  let tv = 0;
  for (let i = 0; i < warm; i++) tv += typedValues(new Int16Array([1, 2, i]));
  show('typedValues', tv);
  let ag = 0;
  for (let i = 0; i < warm; i++) ag += argumentsIteration(1, 2, i);
  show('argumentsIteration', ag);
  let sb = 0;
  for (let i = 0; i < warm; i++) sb += subclassIteration(Bag.from([1, 2, i]));
  show('subclassIteration', sb);

  show('osrInTry', osrInTry(200000));
  show('catchThenLoop', catchThenLoop(4000));

  Object.getPrototypeOf([][Symbol.iterator]()).return = function () {
    arrayReturns++;
    return {};
  };
  show('arraysAfterReturn', [arrayCases(), arrayReturns]);
  let pi = 0;
  for (let i = 0; i < warm * 2; i++) pi += patchedIterator([1, 2, 3]);
  const original = Array.prototype[Symbol.iterator];
  Array.prototype[Symbol.iterator] = function* () { yield 10; yield 20; };
  pi += patchedIterator([1, 2, 3]);
  Array.prototype[Symbol.iterator] = original;
  pi += patchedIterator([1, 2, 3]);
  show('patchedIterator', pi);

  finallyRuns = 0;
  let fn = 0;
  for (let i = 0; i < warm * 2; i++) fn += finallyNormal(i);
  show('finallyNormal', [fn, finallyRuns]);
  finallyRuns = 0;
  const rtf = [];
  for (let i = 0; i < warm * 2; i++) rtf.push(returnThroughFinally(i));
  show('returnThroughFinally', [rtf.slice(-3), finallyRuns]);
  const fo = [];
  for (let i = 0; i < warm * 2; i++) fo.push(finallyOverridesReturn(i));
  show('finallyOverridesReturn', fo.slice(-4));
  finallyRuns = 0;
  const ttf = [];
  for (let i = 0; i < warm * 2; i++) ttf.push(throwThroughFinally(i));
  show('throwThroughFinally', [ttf.slice(-6), finallyRuns]);
  let sw = 0;
  for (let i = 0; i < warm * 2; i++) sw += finallySwallows(i).length;
  show('finallySwallows', sw);
  finallyRuns = 0;
  show('breakContinueFinally', [breakContinueFinally(3000), finallyRuns]);
  const nf = [];
  for (let i = 0; i < warm * 2; i++) nf.push(nestedFinally(i));
  show('nestedFinally', nf.slice(-2));
  const caf = [];
  for (let i = 0; i < warm * 2; i++) caf.push(catchAndFinally(i));
  show('catchAndFinally', caf.slice(-3));
  show('loopInFinally', loopInFinally(100000));
  finallyRuns = 0;
  show('loopInFinallyAfterThrow', loopInFinallyAfterThrow(5000));
  closes = 0;
  let fif = 0;
  for (let i = 0; i < warm * 2; i++) fif += forOfInFinally(countedIterable(4, 0, false), i % 5);
  show('forOfInFinally', [fif, closes]);
  closes = 0;
  finallyRuns = 0;
  let rff = 0;
  for (let i = 0; i < warm * 2; i++) rff += returnFromForOfThroughFinally(countedIterable(4, 0, false), i % 6);
  show('returnFromForOfThroughFinally', [rff, finallyRuns, closes]);
  show('labeledThroughFinally', labeledThroughFinally(500));
  const tff = [];
  for (let i = 0; i < warm * 2; i++) tff.push(throwFromFinally(i));
  show('throwFromFinally', tff.slice(-4));
  let eif = 0;
  for (let i = 0; i < warm * 2; i++) eif += exitInFinally(i, i < warm * 2 - 10 ? { v: 1 } : { w: 0, v: 2.5 });
  show('exitInFinally', eif);

  finallyRuns = 0;
  const prx = [];
  for (let i = 0; i < warm * 2; i++) prx.push(pendingReturnExit(i, i < warm * 2 - 5 ? { v: 1 } : { w: 0, v: 2.5 }));
  show('pendingReturnExit', [prx.slice(-6), finallyRuns]);
  let pjx = 0;
  for (let i = 0; i < warm; i++) pjx += pendingJumpExit(20, i < warm - 3 ? { v: 1 } : { w: 1, v: 0.5 });
  show('pendingJumpExit', pjx);
  finallyRuns = 0;
  const npx = [];
  for (let i = 0; i < warm * 2; i++) npx.push(nestedPendingReturn(i, i < warm * 2 - 4 ? { a: 'x', b: 1 } : { z: 0, a: 'yy', b: 0.5 }));
  show('nestedPendingReturn', [npx.slice(-5), finallyRuns]);
  finallyRuns = 0;
  const pcx = [];
  for (let i = 0; i < warm * 2; i++) pcx.push(pendingReturnCatchInFinally(i, { t: i % 9 === 0 }));
  show('pendingReturnCatchInFinally', [pcx.slice(-3), finallyRuns]);
  finallyRuns = 0;
  const pix = [];
  for (let i = 0; i < warm * 2; i++) pix.push(pendingReturnInlinedExit(i, i < warm * 2 - 3 ? { v: 1 } : { q: 1, v: 0.25 }));
  show('pendingReturnInlinedExit', [pix.slice(-4), finallyRuns]);
  const rjx = [];
  for (let i = 0; i < warm * 2; i++) rjx.push(returnInFinallyOverridesJump(i % 9));
  show('returnInFinallyOverridesJump', rjx.slice(-9));
  const bof = [];
  for (let i = 0; i < warm * 2; i++) bof.push(breakOutOfFinally(i));
  show('breakOutOfFinally', bof.slice(-2));

  const oo1 = new OptOnly(3);
  let ooTotal = 0;
  for (let i = 0; i < warm * 2; i++) ooTotal += oo1.scaled([1, 2, i % 6, 4], i % 4 === 0 ? 4 : -1);
  const oo2 = new OptOnly(0.5);
  oo2.extra = 1;
  for (let i = 0; i < 50; i++) ooTotal += oo2.scaled(new Set([1, 2, i % 3]), 2);
  show('optOnly', ooTotal);

  let boxes = null;
  for (let k = 0; k < warm; k++) boxes = [boxedOutside(200), capturedInCatch(200), unrelatedClosure(200)];
  show('boxes', boxes);

  closes = 0;
  let ex = 0;
  for (let i = 0; i < warm * 2; i++) ex += exhausted(countedIterable(i % 4, 0, false));
  show('exhausted', [ex, closes]);
  closes = 0;
  let ntd = 0;
  for (let i = 0; i < warm * 2; i++) ntd += nextThrowsDestructure(countedIterable(4, i % 3 ? 0 : 2, false));
  show('nextThrowsDestructure', [ntd, closes]);

  const pending = [];
  for (let i = 0; i < 30; i++) pending.push(asyncTry(i));
  Promise.all(pending).then(values => {
    show('asyncTry', values.slice(-6));
    console.log(out.join('\n'));
  });
}

const COMPILED = [
  'catchEvery', 'lateCatch', 'nested', 'noBinding', 'rethrow', 'unboxedLive', 'capturedInTry', 'argumentsInTry', 'fewArgs',
  'getterThrows', 'nativeThrows', 'outer', 'stackOf', 'sumOf', 'pairs', 'holesAndDefaults', 'throwingDefault', 'notIterable',
  'sumGen', 'catchGen', 'restFromGen', 'mapEntries', 'setValues', 'reversed', 'mixedIterables', 'breakEarly', 'throwInside',
  'returnInside', 'labeled', 'catchReturnThrow', 'holey', 'typedValues', 'subclassIteration', 'osrInTry', 'catchThenLoop',
  'patchedIterator', 'finallyNormal', 'returnThroughFinally', 'finallyOverridesReturn', 'throwThroughFinally', 'finallySwallows',
  'breakContinueFinally', 'nestedFinally', 'catchAndFinally', 'loopInFinally', 'forOfInFinally', 'returnFromForOfThroughFinally',
  'labeledThroughFinally', 'throwFromFinally', 'exitInFinally', 'boxedOutside', 'capturedInCatch', 'unrelatedClosure',
  'pendingReturnExit', 'pendingJumpExit', 'nestedPendingReturn', 'pendingReturnCatchInFinally', 'pendingReturnInlinedExit',
  'returnInFinallyOverridesJump', 'breakOutOfFinally',
  'exhausted', 'nextThrowsDestructure', 'arrayHead', 'arrayExact', 'arrayShort', 'arrayBreak',
];

if (process.argv[2] === 'child') {
  runCases();
} else {
  const { spawnSync } = require('node:child_process');
  const base = { ...process.env };
  delete base.ANT_DEBUG;
  const run = (env, args = []) => spawnSync(process.execPath, [...args, __filename, 'child'], {
    encoding: 'utf8',
    env: { ...base, ...env, NO_COLOR: '1' },
    timeout: 120000, maxBuffer: 16 * 1024 * 1024,
  });
  const interp = run({}, ['--jitless']);
  assert.strictEqual(interp.status, 0, interp.stdout + interp.stderr);

  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  const modes = [
    ['default', 'dump/vm:op-warn dump/opt:stats'],
    ['verify', 'opt:verify dump/vm:op-warn'],
    ['force', 'opt:force,verify dump/vm:op-warn'],
    ['off', 'opt:off dump/vm:op-warn'],
  ];
  for (const [name, debug] of modes) {
    const child = run({ ANT_DEBUG: debug });
    assert.strictEqual(child.status, 0, `${name}: ${child.error || child.stdout + child.stderr}`);
    assert.strictEqual(child.stdout, interp.stdout, `${name}: output differs from the interpreter`);
    assert.doesNotMatch(child.stderr, /codegen-failed func=/, `${name}: codegen failed`);
    if (tier && (name === 'default' || name === 'verify')) {
      for (const fn of COMPILED) {
        assert.match(child.stderr, new RegExp(`jit: opt compiled func=${fn} `), `${name}: ${fn} must compile on the opt tier`);
        assert.doesNotMatch(child.stderr, new RegExp(`jit: opt rejected func=${fn} `), `${name}: ${fn} must not be rejected`);
      }
      assert.doesNotMatch(child.stderr, /reason=op line/, `${name}: no function is rejected for an unsupported op`);
      assert.doesNotMatch(child.stderr, /jit: opt dropped func=catchEvery reason=(?!dead-value)/, `${name}: catching never drops catchEvery`);
      assert.doesNotMatch(child.stderr, /jit: bailout .*func=catchEvery /, `${name}: catching never exits catchEvery`);
      assert.match(child.stderr, /jit: opt dropped func=lateCatch reason=catch-without-entry/, `${name}: lateCatch recompiles with a catch entry`);
    }
    if (tier && name === 'default') {
      const stats = /caught_in_code=(\d+) caught_in_interpreter=(\d+)/.exec(child.stderr);
      assert.ok(stats, 'catch stats are printed');
      assert.ok(Number(stats[1]) > 1000, `most catches stay in compiled code: ${stats[0]}`);
      assert.match(child.stderr, / unwinds=0\n/, 'return, break and continue through finally complete in compiled code');
    }
    console.log(`PASS opt try ${name}`);
  }
}
