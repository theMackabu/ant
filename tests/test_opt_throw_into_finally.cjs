const assert = require('node:assert');

const cases = [];
function add(name, expected, fn) {
  cases.push({ name, expected, fn });
}

function mix(h, x) {
  return (Math.imul(h, 31) + (x | 0)) | 0;
}

add('throw to finally then catch', 9000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw i; } finally { c++; }
    } catch (e) { c += e === i ? 2 : 100; }
  }
  return c;
});

add('rare throw to finally then catch', 6188, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { if ((i & 15) === 3) throw i; c++; } finally { c++; }
    } catch (e) { c += e === i ? 2 : 100; }
  }
  return c;
});

add('three nested finally', 15000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try {
        try {
          try { throw i; } finally { c += 1; }
        } finally { c += 1; }
      } finally { c += 1; }
    } catch (e) { c += e === i ? 2 : 100; }
  }
  return c;
});

add('finally sees writes made before the throw', 570282168, function (n) {
  let h = 0;
  for (let i = 0; i < n; i++) {
    let a = 1, b = 'x';
    try {
      try {
        a = i;
        b = 'y';
        if (i % 3 !== 0) throw a;
        a = -1;
        b = 'z';
      } finally { h = mix(h, a + b.charCodeAt(0)); }
    } catch (e) { h = mix(h, e + a); }
  }
  return h;
});

add('finally return overrides the throw', 6000, function (n) {
  function inner(i) {
    try { throw i; } finally { return i + 1; }
  }
  let c = 0;
  for (let i = 0; i < n; i++) c += inner(i) - i;
  for (let i = 0; i < n; i++) {
    const r = (function () { try { if (i >= 0) throw i; } finally { if (i >= 0) return 1; } return 5; })();
    c += r;
  }
  return c;
});

add('finally break and continue override the throw', 9000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try { throw i; } finally { c++; continue; }
  }
  for (let i = 0; i < n; i++) {
    for (;;) {
      try { throw i; } finally { c++; break; }
    }
    c++;
  }
  return c;
});

add('finally labelled break overrides the throw', 6000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    out: {
      try { throw i; } finally { c++; break out; }
    }
    c++;
  }
  return c;
});

add('finally throws a different value', 3000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw i; } finally { throw -i - 1; }
    } catch (e) { c += e === -i - 1 ? 1 : 100; }
  }
  return c;
});

add('finally throws only sometimes', 12000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw i; } finally { if (i & 1) throw 'odd'; c++; }
    } catch (e) { c += e === i ? 2 : e === 'odd' ? 5 : 100; }
  }
  return c;
});

add('try catch inside the finally body keeps the pending throw', 15000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw i; } finally {
        try { throw 'inner'; } catch (e) { c += e === 'inner' ? 1 : 100; }
        c++;
      }
    } catch (e) { c += e === i ? 3 : 1000; }
  }
  return c;
});

add('try finally inside the finally body', 18000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw i; } finally {
        try {
          try { throw i + 0.5; } finally { c++; }
        } catch (e) { c += e === i + 0.5 ? 1 : 100; }
        c++;
      }
    } catch (e) { c += e === i ? 3 : 1000; }
  }
  return c;
});

add('rethrow after finally reaches the caller', 9000, function (n) {
  function inner(i) {
    try { throw i; } finally { inner.count++; }
  }
  inner.count = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    try { inner(i); c += 100; } catch (e) { c += e === i ? 2 : 100; }
  }
  return c + inner.count;
});

add('mixed completions through one finally', -847528378, function (n) {
  function inner(i, log) {
    for (let k = 0; k < 3; k++) {
      try {
        if (i % 7 === 0) return 'r' + k;
        if (i % 7 === 1) throw 't' + k;
        if (i % 7 === 2) break;
        if (i % 7 === 3) continue;
        log.push(k);
      } finally { log.push(-1); }
    }
    return 'end';
  }
  let h = 0;
  for (let i = 0; i < n; i++) {
    const log = [];
    let r;
    try { r = inner(i, log); } catch (e) { r = 'caught ' + e; }
    h = mix(h, r.length + log.length * 7 + r.charCodeAt(0));
  }
  return h;
});

add('mixed completions through nested finally', -1638554276, function (n) {
  let h = 0;
  function inner(i) {
    let s = 0;
    try {
      try {
        if (i % 5 === 0) throw i;
        if (i % 5 === 1) return -1;
        s += 1;
      } finally { s += 10; h = mix(h, s); }
      s += 100;
    } finally { s += 1000; h = mix(h, s); }
    return s;
  }
  for (let i = 0; i < n; i++) {
    try { h = mix(h, inner(i)); } catch (e) { h = mix(h, e + 7); }
  }
  return h;
});

add('loop inside the finally body with a pending throw', 21000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { if (i % 2) throw i; } finally { for (let k = 0; k < 3; k++) c++; }
      c++;
    } catch (e) { c += e === i ? 7 : 1000; }
  }
  return c;
});

add('throw out of a loop into finally', 24000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try {
        for (let k = 0; ; k++) { c++; if (k === 2) throw k + i; }
      } finally { c++; }
    } catch (e) { c += e === i + 2 ? 4 : 1000; }
  }
  return c;
});

add('throw from an inlined callee into finally', 9000, function (n) {
  function thrower(i) { if (i >= 0) throw i; return 0; }
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { thrower(i); c += 100; } finally { c++; }
    } catch (e) { c += e === i ? 2 : 100; }
  }
  return c;
});

add('throw from two inlined levels into finally', 9000, function (n) {
  function leaf(i) { throw { i }; }
  function mid(i) { return leaf(i) + 1; }
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { mid(i); c += 100; } finally { c++; }
    } catch (e) { c += e.i === i ? 2 : 100; }
  }
  return c;
});

add('throw from a call that is not inlined into finally', 9000, function (n) {
  function deep(i, d) {
    if (d === 0) throw i;
    try { return deep(i, d - 1); } finally { deep.count++; }
  }
  deep.count = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { deep(i, 1); c += 100; } finally { c++; }
    } catch (e) { c += e === i ? 1 : 100; }
  }
  return c + deep.count;
});

add('builtin throws into finally', 13000, function (n) {
  let c = 0;
  const bad = [null, undefined];
  for (let i = 0; i < n; i++) {
    try {
      try {
        if (i % 3 === 0) bad[i & 1].x;
        else if (i % 3 === 1) JSON.parse('{');
        else bad[0]();
        c += 100;
      } finally { c++; }
    } catch (e) { c += e instanceof TypeError ? 3 : e instanceof SyntaxError ? 4 : 100; }
  }
  return c;
});

add('finally handler entered from a call in a hot loop', 6375, function (n) {
  function maybe(i) {
    if ((i & 7) === 5) throw new RangeError('r' + i);
    return 1;
  }
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { c += maybe(i); } finally { c++; }
    } catch (e) { c += e.message === 'r' + i ? 2 : 1000; }
  }
  return c;
});

add('values of every type keep their identity', 99000, function (n) {
  const sym = Symbol('s');
  const obj = { a: 1 }, arr = [1, 2], fn = function () {}, err = new Error('e');
  const values = [undefined, null, true, false, 0, -0, 1.5, NaN, Infinity, -Infinity, 2 ** 31, -(2 ** 31), 'str', '', 10n, sym, obj, arr, fn, err, new TypeError('t'), /re/];
  let c = 0;
  for (let i = 0; i < n; i++) {
    const v = values[i % values.length];
    let seen = 0;
    try {
      try { throw v; } finally { seen = 1; }
    } catch (e) { c += Object.is(e, v) ? 10 + seen : 1000; }
    try {
      try { try { throw v; } finally { seen++; } } finally { seen++; }
    } catch (e) { c += Object.is(e, v) && seen === 3 ? 22 : 1000; }
  }
  return c;
});

add('error objects keep message and stack', 9000, function (n) {
  function make(i) { return new Error('boom ' + i); }
  let c = 0;
  for (let i = 0; i < n; i++) {
    const err = make(i);
    const before = err.stack;
    try {
      try { throw err; } finally { c++; }
    } catch (e) {
      if (e === err && e.message === 'boom ' + i && e.stack === before && typeof before === 'string' && before.includes('boom ' + i)) c += 2;
      else c += 1000;
    }
  }
  return c;
});

add('errors created in the try body carry a stack through finally', 6000, function (n) {
  let c = 0;
  function thrower(i) {
    try { throw new Error('made ' + i); } finally { c++; }
  }
  for (let i = 0; i < n; i++) {
    try { thrower(i); } catch (e) {
      const stack = String(e.stack);
      c += stack.includes('made ' + i) && stack.includes('thrower') ? 1 : 1000;
    }
  }
  return c;
});

add('allocation in the handler keeps the thrown object alive', 853075006, function (n) {
  let h = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw { id: i, tag: 'k' + i, list: [i, i + 1] }; } finally {
        const junk = [];
        for (let k = 0; k < 24; k++) junk.push({ k, s: 'x' + k + i, a: [k, i] });
        h = mix(h, junk.length + junk[23].a[1]);
      }
    } catch (e) { h = mix(h, e.id + e.list[1] + e.tag.length); }
  }
  return h;
});

add('captured variables written before the throw', -1381652880, function (n) {
  let h = 0;
  for (let i = 0; i < n; i++) {
    let x = i;
    const get = () => x;
    try {
      try { x = i * 2; throw get; } finally { h = mix(h, get()); x++; }
    } catch (e) { h = mix(h, e() + (e === get ? 1 : 100)); }
  }
  return h;
});

add('closure created in the try body before the throw', -1381652880, function (n) {
  let h = 0;
  for (let i = 0; i < n; i++) {
    let x = i;
    let get;
    try {
      try { get = () => x; x = i * 2; throw get; } finally { h = mix(h, get()); x++; }
    } catch (e) { h = mix(h, e() + (e === get ? 1 : 100)); }
  }
  return h;
});

add('arguments thrown and read around finally', 12000, function (n) {
  function thrower() { throw arguments; }
  function own(a, b) {
    try {
      try { throw arguments; } finally { own.count++; }
    } catch (e) { return e.length + e[1]; }
  }
  own.count = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { thrower(i, 1); } finally { c++; }
    } catch (e) { c += e.length === 2 && e[0] === i ? 1 : 1000; }
    c += own(i, -1) === 1 ? 1 : 1000;
  }
  return c + own.count;
});

add('this and new.target survive the handler', 9000, function (n) {
  function Box(i) {
    try {
      try { if (new.target) throw this; } finally { this.seen = i; }
    } catch (e) { e.caught = e === this; }
  }
  let c = 0;
  for (let i = 0; i < n; i++) {
    const box = new Box(i);
    c += box.seen === i && box.caught === true ? 3 : 1000;
  }
  return c;
});

add('exit inside the finally body reached by a throw', 3016, function (n) {
  function inner(i, v) {
    try { throw i; } finally { inner.acc = inner.acc + v; }
  }
  inner.acc = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    const v = i === n - 7 ? 'S' : i > n - 7 ? 'x' : 1;
    try { inner(i, v); c += 1000; } catch (e) { c += e === i ? 1 : 1000; }
  }
  return c + String(inner.acc).length + (inner.acc === '2993Sxxxxxx' ? 5 : 0);
});

add('exit inside the finally body in the same frame', 3009, function (n) {
  let acc = 0, c = 0;
  for (let i = 0; i < n; i++) {
    const v = i === n - 5 ? 'S' : i > n - 5 ? 'x' : 1;
    try {
      try { throw i; } finally { acc = acc + v; }
    } catch (e) { c += e === i ? 1 : 1000; }
  }
  return c + String(acc).length;
});

add('exit inside a nested finally body keeps both completions', 4518, function (n) {
  let acc = 0, c = 0;
  function inner(i, v) {
    try {
      try { if (i % 2) throw i; return -i; } finally { acc = acc + v; }
    } finally { c++; }
  }
  for (let i = 0; i < n; i++) {
    const v = i === n - 9 ? 'S' : i > n - 9 ? 'x' : 1;
    try { c += inner(i, v) === -i ? 1 : 1000; } catch (e) { c += e === i ? 1 : 1000; }
  }
  return c + String(acc).length + (acc === '2991Sxxxxxxxx' ? 5 : 0);
});

add('throw in finally body replaces a pending return', 7500, function (n) {
  function inner(i) {
    try { return i; } finally { if (i % 2) throw 'f' + i; }
  }
  let c = 0;
  for (let i = 0; i < n; i++) {
    try { c += inner(i) === i ? 1 : 1000; } catch (e) { c += e === 'f' + i ? 4 : 1000; }
  }
  return c;
});

add('catch and finally on the same statement', 16500, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw i; } catch (e) { c += e === i ? 1 : 1000; if (i % 2) throw e + 1; } finally { c += 2; }
      c++;
    } catch (e) { c += e === i + 1 ? 4 : 1000; }
  }
  return c;
});

add('switch and conditional throws inside try', 327000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try {
        switch (i % 4) {
          case 0: throw 'a';
          case 1: c += 1; break;
          case 2: throw 2;
          default: c += 3;
        }
        c += 10;
      } finally { c += 100; }
    } catch (e) { c += e === 'a' ? 5 : e === 2 ? 7 : 1000; }
  }
  return c;
});

add('destructuring catch parameter after finally', 13500, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { throw { a: i, b: [1, 2] }; } finally { c++; }
    } catch ({ a, b: [x, y] }) { c += a === i ? x + y : 1000; }
    try {
      try { throw [i]; } finally { c++; }
    } catch { c -= 0.5; }
  }
  return c;
});

add('finally without a throw is unchanged', 9000, function (n) {
  let c = 0;
  for (let i = 0; i < n; i++) {
    try { c++; } finally { c++; }
    try { try { c++; } finally { if (i < 0) throw i; } } catch (e) { c += 1000; }
  }
  return c;
});

add('six completion kinds through one finally', -795007562, function (n) {
  function inner(i) {
    let s = 0;
    a: for (let x = 0; x < 2; x++) {
      b: for (let y = 0; y < 2; y++) {
        for (let z = 0; z < 2; z++) {
          try {
            const k = (i + x * 4 + y * 2 + z) % 7;
            if (k === 0) continue a;
            if (k === 1) break b;
            if (k === 2) continue;
            if (k === 3) break;
            if (k === 4) return s + 1000;
            if (k === 5) throw s;
            s += 1;
          } finally { s += 10; }
        }
        s += 100;
      }
    }
    return s;
  }
  let h = 0;
  for (let i = 0; i < n; i++) {
    try { h = mix(h, inner(i)); } catch (e) { h = mix(h, e + 5000); }
  }
  return h;
});

add('six completion kinds inside a finally body reached by a throw', -795007562, function (n) {
  function inner(i) {
    let s = 0;
    const base = i;
    try {
      try { if (base >= 0) throw 'first'; } finally {
        a: for (let x = 0; x < 2; x++) {
          b: for (let y = 0; y < 2; y++) {
            for (let z = 0; z < 2; z++) {
              try {
                const k = (base + x * 4 + y * 2 + z) % 7;
                if (k === 0) continue a;
                if (k === 1) break b;
                if (k === 2) continue;
                if (k === 3) break;
                if (k === 4) return s + 1000;
                if (k === 5) throw s;
                s += 1;
              } finally { s += 10; }
            }
            s += 100;
          }
        }
      }
    } catch (e) { return e === 'first' ? s : e + 5000; }
    return -1;
  }
  let h = 0;
  for (let i = 0; i < n; i++) h = mix(h, inner(i));
  return h;
});

add('more finally blocks than the native limit', 103000, function (n) {
  function thrower(i) {
    try { throw i; } finally { thrower.count++; }
  }
  thrower.count = 0;
  function nine(i) {
    let s = 0;
    try { s += 1; } finally { s += 2; }
    try { s += 1; } finally { s += 2; }
    try { s += 1; } finally { s += 2; }
    try { s += 1; } finally { s += 2; }
    try { s += 1; } finally { s += 2; }
    try { s += 1; } finally { s += 2; }
    try { s += 1; } finally { s += 2; }
    try { s += 1; } finally { s += 2; }
    try {
      try {
        if (i % 3 === 0) throw i;
        if (i % 3 === 1) thrower(i);
        s += 1;
      } finally { s += 5; }
    } catch (e) { s += e === i ? 7 : 1000; }
    return s;
  }
  let c = 0;
  for (let i = 0; i < n; i++) c += nine(i);
  return c + thrower.count;
});

add('throw inside for of with try around and inside', 386703450, function (n) {
  const items = [1, 2, 3];
  let h = 0;
  for (let i = 0; i < n; i++) {
    try {
      for (const item of items) {
        try { if (item === 2 && i % 2) throw item + i; h = mix(h, item); } finally { h = mix(h, 7); }
      }
    } catch (e) { h = mix(h, e); }
    for (const item of items) {
      try {
        try { if (item === (i % 4)) throw item; } finally { h = mix(h, item); }
      } catch (e) { h = mix(h, e + 100); continue; }
      h = mix(h, 1);
    }
  }
  return h;
});

add('loop inside the finally body entered from a call', 23000, function (n) {
  function thrower(i) {
    try { if (i % 3 === 0) throw i; return 1; } finally { thrower.count++; }
  }
  thrower.count = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    try {
      try { c += thrower(i); } finally { for (let k = 0; k < 3; k++) c++; }
      c++;
    } catch (e) { c += e === i ? 7 : 1000; }
  }
  return c + thrower.count;
});

add('exit inside the finally body entered from a call', 9015, function (n) {
  function thrower(i) {
    try { if (i % 2 === 0) throw i; return 1; } finally { thrower.count++; }
  }
  thrower.count = 0;
  let acc = 0, c = 0;
  for (let i = 0; i < n; i++) {
    const v = i === n - 6 ? 'S' : i > n - 6 ? 'x' : 1;
    try {
      try { c += thrower(i); } finally { acc = acc + v; }
    } catch (e) { c += e === i ? 3 : 1000; }
  }
  return c + String(acc).length + (acc === '2994Sxxxxx' ? 5 : 0) + thrower.count;
});

add('one catch reached by a throw and by a call', 10000, function (n) {
  function thrower(i) {
    try { throw 'call' + i; } finally { thrower.count++; }
  }
  thrower.count = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    let step = 0;
    try {
      step = 1;
      if (i % 3 === 0) throw 'own' + i;
      step = 2;
      if (i % 3 === 1) thrower(i);
      step = 3;
    } catch (e) { c += e === (step === 1 ? 'own' : 'call') + i ? step : 1000; }
    c += step;
  }
  return c + thrower.count;
});

add('writes before the throw in a function that reads arguments', 5627253, function (n) {
  function inner(count) {
    'use strict';
    let x = 0, c = 0;
    for (let i = 0; i < count; i++) {
      try {
        try { x = i; throw i; } finally { c += x; }
      } catch (e) { c += x - e + 1; }
    }
    return c + arguments.length;
  }
  return inner(n) + inner(n >> 1, 0);
});

add('inlined constructor throws its receiver', 7500, function (n) {
  function Thing(i) {
    this.i = i;
    if (i % 2) throw this;
  }
  let c = 0;
  for (let i = 0; i < n; i++) {
    let made = null;
    try {
      try { made = new Thing(i); } finally { c++; }
    } catch (e) { c += e instanceof Thing && e.i === i && made === null ? 2 : 1000; }
    c += made ? 1 : 0;
  }
  return c;
});

add('closure made in the finally body sees the thrown value', 1886561008, function (n) {
  let h = 0;
  for (let i = 0; i < n; i++) {
    let seen = -1;
    let read = null;
    try {
      try { seen = i; throw i * 3; } finally { read = () => seen; seen++; }
    } catch (e) { h = mix(h, e + read()); }
  }
  return h;
});

add('call that throws inside the finally body keeps the pending completion', 10500, function (n) {
  function thrower(i) {
    try { throw 'in' + i; } finally { thrower.count++; }
  }
  thrower.count = 0;
  function inner(i) {
    try {
      if (i % 2) throw i;
      return -i;
    } finally {
      try { thrower(i); } catch (e) { inner.seen += e === 'in' + i ? 1 : 1000; }
    }
  }
  inner.seen = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    try { c += inner(i) === -i ? 1 : 1000; } catch (e) { c += e === i ? 2 : 1000; }
  }
  return c + inner.seen + thrower.count;
});

add('throw inside a loop that runs in a finally body', 27200, function (n) {
  function inner(count, step) {
    let c = step;
    try { c++; } finally {
      for (let i = 0; i < count; i++) {
        try { if (i & 1) throw i; c += step; } catch (e) { c += e; }
        c += step;
      }
    }
    return c;
  }
  let total = 0;
  for (let k = 0; k < n; k += 30) total += inner(30, k & 3);
  return total;
});

add('loop in the catch body after the try block captured a variable', 120000, function (n) {
  function inner(count) {
    let x = 0, read = null;
    try { read = () => x; throw 1; } catch (e) { for (let i = 0; i < count; i++) x++; }
    return read() + x;
  }
  let total = 0;
  for (let k = 0; k < 3; k++) total += inner(n * 6 + 2000);
  return total;
});

add('constructor that is not inlined throws into finally', 10500, function (n) {
  function Thing(i) {
    try { this.i = i; if (i % 2) throw this; } finally { Thing.made++; }
  }
  Thing.made = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    let made = null;
    try {
      try { made = new Thing(i); } finally { c++; }
    } catch (e) { c += e instanceof Thing && e.i === i && made === null ? 2 : 1000; }
    c += made ? 1 : 0;
  }
  return c + Thing.made;
});

add('calls with several targets throw into one handler', -1765766109, function (n) {
  function a(i) { try { if (i % 3 === 0) throw 'a' + i; return 1; } finally { a.n++; } }
  function b(i) { try { if (i % 3 === 1) throw 'b' + i; return 2; } finally { b.n++; } }
  const o = { m(i) { try { if (i % 3 === 2) throw 'm' + i; return 3; } finally { a.n++; } } };
  a.n = 0;
  b.n = 0;
  const fns = [a, b];
  let h = 0;
  for (let i = 0; i < n; i++) {
    let step = 0;
    try {
      try {
        h = mix(h, fns[i & 1](i));
        step = 1;
        h = mix(h, o.m(i));
        step = 2;
      } finally { h = mix(h, step); }
    } catch (e) { h = mix(h, e.length + e.charCodeAt(0) + step); }
  }
  return h + a.n + b.n;
});

add('call inside an inlined callee throws to the caller handler', 12000, function (n) {
  function thrower(i) { try { if (i % 2) throw { i }; return i; } finally { thrower.n++; } }
  thrower.n = 0;
  function wrap(i, k) { return thrower(i) + k; }
  let c = 0;
  for (let i = 0; i < n; i++) {
    let local = i * 2;
    try {
      try { local = wrap(i, 1); local += 1000; } finally { c += local & 1; }
    } catch (e) { c += e.i === i && local === i * 2 ? 5 : 100000; }
  }
  return c + thrower.n;
});

add('exit after a call threw into the handler', 6016, function (n) {
  function thrower(i) { try { throw i; } finally { thrower.n++; } }
  thrower.n = 0;
  let acc = 0, c = 0;
  for (let i = 0; i < n; i++) {
    const v = i === n - 6 ? 'S' : i > n - 6 ? 'x' : 1;
    let before = i;
    try {
      try { before = i + 1; thrower(i); before = -1; } finally { acc = acc + v; }
    } catch (e) { acc = acc + v; c += e === i && before === i + 1 ? 1 : 1000; }
  }
  return c + String(acc).length + thrower.n;
});

add('error marker never reaches the continuation', 14250, function (n) {
  function maybe(i) { try { if (i % 4 === 0) throw new RangeError('r' + i); return { v: i }; } finally { maybe.n++; } }
  maybe.n = 0;
  let c = 0;
  for (let i = 0; i < n; i++) {
    let got = null;
    try { got = maybe(i); c += got.v === i ? 1 : 1000; } catch (e) { c += e instanceof RangeError && got === null ? 2 : 1000; }
    try { c += typeof maybe(i + 1).v === 'number' ? 1 : 1000; } catch (e) { c += e.message === 'r' + (i + 1) ? 3 : 1000; }
  }
  return c + maybe.n;
});

function generatorCase(n) {
  function* gen(i) {
    try {
      try { yield 1; throw i; } finally { yield 2; }
    } catch (e) { yield e; }
  }
  let c = 0;
  for (let i = 0; i < n; i++) {
    const values = [...gen(i)];
    c += values.length === 3 && values[2] === i ? 1 : 1000;
  }
  return c;
}

async function asyncCase(n) {
  async function inner(i) {
    try {
      try { await null; throw i; } finally { await null; }
    } catch (e) { return e; }
  }
  let c = 0;
  for (let i = 0; i < n; i++) c += (await inner(i)) === i ? 1 : 1000;
  return c;
}

function statsChild() {
  function local(n) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      try {
        try { throw i; } finally { c++; }
      } catch (e) { c += e === i ? 1 : 1000; }
    }
    return c;
  }
  function nested(n) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      try {
        try {
          try { if (i & 1) throw i; } finally { for (let k = 0; k < 2; k++) c++; }
        } finally { if (i & 2) throw -i; }
      } catch (e) { c += e === i || e === -i ? 1 : 1000; }
    }
    return c;
  }
  function inBody(n) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      try {
        try { throw i; } finally {
          try { throw 'in'; } catch (e) { c += e === 'in' ? 1 : 1000; }
        }
      } catch (e) { c += e === i ? 1 : 1000; }
    }
    return c;
  }
  function loopInFinally(n) {
    let x = 0, c = 0;
    try { c++; } finally {
      for (let i = 0; i < n; i++) {
        try { x = i; throw i; } catch (e) { c += x - e + 1; }
      }
    }
    return c;
  }
  function coldCatch(n, o) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      try { c++; } finally {
        try { if ((i & 7) === 3) throw i; c++; } catch (e) { delete o.p; }
      }
    }
    return c;
  }
  function finallyInBody(n) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      try { c++; } finally {
        try {
          try { throw i; } finally { c++; }
        } catch (e) { c += e === i ? 1 : 1000; }
      }
    }
    return c;
  }
  function closureInTry(n) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      let x = i, get = null;
      try { get = () => x; x++; throw get; } catch (e) { c += e() - i; }
    }
    return c;
  }
  function thrower(i) {
    try { if (i % 3 === 0) throw i; return 1; } finally { thrower.count++; }
  }
  thrower.count = 0;
  function called(n) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      try {
        try { c += thrower(i); } finally { c++; }
      } catch (e) { c += e === i ? 1 : 1000; }
    }
    return c;
  }
  function fromAccess(n, nothing) {
    let c = 0;
    for (let i = 0; i < n; i++) {
      try {
        try { c += nothing.length; } finally { c++; }
      } catch (e) { c += e instanceof TypeError ? 1 : 1000; }
    }
    return c;
  }
  let total = 0;
  for (let round = 0; round < 40; round++) {
    total += fromAccess(100, null);
    total += local(400) + nested(400) + inBody(400) + coldCatch(400, { p: 1 }) + closureInTry(400) + finallyInBody(400) + called(450);
    for (let k = 0; k < 50; k++) total += loopInFinally(8);
  }
  console.log(total);
}

function checkStats() {
  const tier = (process.arch === 'arm64' || process.arch === 'x64') && (process.platform === 'darwin' || process.platform === 'linux');
  if (typeof Ant === 'undefined' || !tier) return;
  const { spawnSync } = require('node:child_process');
  const child = spawnSync(process.execPath, [__filename, 'stats'], {
    encoding: 'utf8',
    env: { ...process.env, ANT_DEBUG: 'dump/opt:stats dump/vm:op-warn', NO_COLOR: '1' },
    timeout: 120000,
  });
  assert.strictEqual(child.status, 0, child.stdout + child.stderr.slice(-4000));
  assert.strictEqual(child.stdout.trim(), '264000');
  const stats = /caught_in_code=(\d+) caught_in_interpreter=(\d+)/.exec(child.stderr);
  assert.match(child.stderr, /jit: opt dropped func=finallyInBody reason=catch-without-entry/, 'a handler inside a finally body learns its throws');
  const rejected = / rejected=(\d+)/.exec(child.stderr);
  assert.ok(stats && rejected, child.stderr.slice(-4000));
  const fromAccesses = 40 * 100;
  assert.ok(Number(stats[1]) >= fromAccesses / 2 && Number(stats[1]) <= fromAccesses, 'throws caught through the catch site: ' + stats[1]);
  assert.ok(Number(stats[2]) <= 32, 'throws that left compiled code: ' + stats[2]);
  assert.strictEqual(rejected[1], '0', 'functions the optimizing tier gave up on');
}

function main() {
  const rounds = 6, count = 3000;
  for (const { name, expected, fn } of cases) {
    for (let round = 0; round < rounds; round++) assert.strictEqual(fn(count), expected, name + ' round ' + round);
  }

  function uncaught(i) {
    try { throw new Error('out ' + i); } finally { uncaught.count++; }
  }
  uncaught.count = 0;
  for (let i = 0; i < 20000; i++) {
    let seen = null;
    try { uncaught(i); } catch (e) { seen = e; }
    assert.ok(seen instanceof Error && seen.message === 'out ' + i);
  }
  assert.strictEqual(uncaught.count, 20000);

  for (let round = 0; round < 3; round++) assert.strictEqual(generatorCase(1000), 1000);

  asyncCase(600).then((value) => {
    assert.strictEqual(value, 600);
    checkStats();
    console.log('ok');
  });
}

if (process.argv[2] === 'stats') statsChild();
else main();
