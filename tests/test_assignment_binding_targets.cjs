const assert = require('node:assert');

function rejects(params, list) {
  for (const src of list) assert.throws(() => new Function(...params, src), SyntaxError, src);
}

function accepts(params, list) {
  for (const src of list) assert.doesNotThrow(() => new Function(...params, src), src);
}

rejects(['o', 'x'], [
  'o?.a = 1', 'o?.a.b = 1', 'o.a?.b = 1', "o?.['a'] = 1", "o?.a['b'] = 1", 'o?.f().x = 1', 'o.f?.().x = 1',
  '(o?.a) = 1', '(o?.a.b) = 1',
  'o?.a += 1', 'o?.a ||= 1', 'o?.a &&= 1', 'o?.a ??= 1', 'o?.a.b **= 2',
  'o?.a++', 'o?.a--', '++o?.a', '--o?.a', '++o?.a.b', 'o?.a.b--',
  '[o?.a] = [1]', '[o?.a, x] = [1, 2]', '[o?.a.b] = [1]', '[...o?.a] = [1]', '[o?.a = 2] = []',
  '({ x: o?.a } = { x: 1 })', '({ x: o?.a = 3 } = {})', '({ ...o?.a } = {})',
  '[[o?.a]] = [[1]]', '({ x: [o?.a] } = { x: [1] })', '[(o?.a)] = [1]',
  'for (o?.a of [1]);', 'for (o?.a in { k: 1 });', 'for (o?.a.b of [1]);',
  'for ([o?.a] of [[1]]);', 'for ({ x: o?.a } in { k: 1 });', 'for ((o?.a) of [1]);',
  '(async () => { for await (o?.a of [1]); })',
  '([o?.a]) => 1', '(o?.a) => 1', 'async (o?.a) => 1',
]);

rejects(['o', 'x', 'y'], [
  'o.f() &&= 1', 'o.f() ||= 1', 'o.f() ??= 1', 'x + y = 1', '1 = 1', 'this = 1', '`t` = 1', '(() => 1) = 1', 'new.target = 1',
  'x++ = 1', '++x = 1', '1++', '++1', '(x + y)++', '--this', 'o.f()?.a++',
  '[o.f()] = [1]', '[1] = [1]', '[x + y] = [1]', '[this] = [1]', '[`t`] = [1]', '[() => 1] = [1]',
  '({ a: 1 } = {})', '({ a: o.f() } = {})', '({ a: x + y } = {})', '({ a() {} } = {})', '({ get a() {} } = {})',
  '[...x, y] = [1]', '[...x,] = [1]', '[...x = 1] = [1]', '({ ...{ a } } = {})', '({ ...[a] } = {})', '({ ...x, y } = {})',
  '([x]) = [1]', '({ x }) = { x: 1 }', '[([x])] = [[1]]', '[({ x })] = [{ x: 1 }]', '({ a: ({ x }) } = { a: {} })',
  'for (1 of []);', 'for (x + y in {});', 'for (([x]) of []);', 'for (this of []);',
  '[x] += [1]', '({ x } ||= {})', '[x]++', '++[x]',
]);

accepts(['o', 'x', 'y'], [
  '(o?.a).b = 5', '(o?.a).b += 5', '(o?.a).b ??= 5', '(o?.a).b++', '--(o?.a).b', '(o?.a)[x] = 1', '(o?.a)().b = 1',
  '[(o?.a).b] = [6]', '({ x: (o?.a).b } = { x: 6 })', '[...(o?.a).b] = [6]',
  'for ((o?.a).b of [7]);', 'for ((o?.a).b in { k: 1 });', 'for ([(o?.a).b] of [[7]]);',
  'x = 1', '(x) = 1', '((x)) = 1', 'o.a = 1', '(o.a) = 1', '(o).a = 1', 'o.a.b.c = 1', 'o[x] = 1', '(o[x]) = 1', 'o.f().a = 1', 'o.f()[x] = 1',
  'this.a = 1', 'this[x] = 1', 'x += 1', 'o.a ||= 1', '(x) &&= 1', 'x++', '(x)--', '++(o.a)', '--o[x]', 'o.f().a++',
  '[x, y] = [1, 2]', '[(x), (o.a)] = [1, 2]', '[x = 1, [y] = [2], ...o.a] = []', '[, x, , y] = []', '[x, y,] = []',
  '({ x, y } = o)', '({ x = 1, a: y = 2, b: { c: o.d }, ...o.e } = o)', '({ a: (x), b: (o.a) } = o)', '({ ...(x) } = o)', "({ 'a': x, 1: y, [x]: o.z } = o)",
  '[{ x }, [y]] = [{}, []]', '[x, ...[y, ...o.a]] = [1, 2, 3]', '[x, ...{ length: y }] = [1, 2]',
  'for (x of []);', 'for ((x) of []);', 'for (o.a of []);', 'for (o[x] in {});', 'for ([x, y] of []);', 'for ({ x, a: o.b } of []);', 'for (this.a of []);',
  'eval = 1', 'arguments = 1', '[eval, arguments] = [1, 2]', 'eval++', 'async = 1', '[async] = [1]', 'o.f() = 1', 'o.f() += 1', 'o.f()++', 'for (o.f() of []);',
  'undefined = 1', '[undefined] = [1]', '({ a: undefined } = o)', 'for (undefined of []);',
]);

rejects(['o'], [
  '"use strict"; eval = 1', '"use strict"; arguments = 1', '"use strict"; [eval] = [1]', '"use strict"; ({ a: arguments } = {})',
  '"use strict"; eval++', '"use strict"; --arguments', '"use strict"; for (eval of []);', '"use strict"; ({ eval } = {})',
  '"use strict"; ([eval]) => 1', '"use strict"; (arguments) => 1', '"use strict"; ({ a: eval }) => 1',
]);
accepts(['o'], ['"use strict"; eval.a = 1', '"use strict"; arguments[0] = 1', '"use strict"; [o.eval, o.arguments] = [1, 2]']);

rejects(['o', 'b'], [
  'var { a: o.x } = { a: 1 }', 'let { a: o.x } = { a: 1 }', 'const { a: o.x } = { a: 1 }',
  'var [o.x] = [1]', 'let [o.x] = [1]', 'const [o.x] = [1]', "var { a: o['x'] } = { a: 1 }", 'let [o[0]] = [1]',
  'var [...o.x] = [1]', 'let { ...o.x } = {}', 'var { a: o.x = 2 } = {}', 'const [o.x = 2] = []',
  'var { a: { b: o.x } } = { a: { b: 1 } }', 'let [[o.x]] = [[1]]', 'var { a: o.x.y } = { a: 1 }', 'var { a: o.f().x } = { a: 1 }',
  'var { a: o?.x } = { a: 1 }', 'var { a: (o.x) } = { a: 1 }', 'var { a: (c) } = { a: 1 }', 'let [(c)] = [1]', 'var [(c) = 1] = []',
  'var { a: f() } = { a: 1 }', 'var [1] = [1]', 'var { a: c + d } = { a: 1 }', 'var [this] = [1]', 'let { a: this.x } = { a: 1 }',
  'var [...c, d] = []', 'var [...c,] = []', 'var [...c = 1] = []', 'var { ...{ c } } = {}', 'var { ...[c] } = {}', 'var { ...c, d } = {}',
  'var { a() {} } = {}', 'var { get a() {} } = {}', 'var [c]', 'let { c }', 'var [c], d = 1',
  'function f({ a: o.x }) {}', 'function f([o.x]) {}', 'function f(...[o.x]) {}', 'function f({ a: o.x } = {}) {}', "(function ({ a: o['x'] }) {})",
  'function f(...a, c) {}', 'function f(...a,) {}', 'function f(...a = []) {}', 'function f([(c)]) {}',
  '({ a: o.x }) => 1', '([o.x]) => 1', '([o.x] = []) => 1', '(...[o.x]) => 1', 'async ({ a: o.x }) => 1', 'async ([o.x]) => 1',
  '(...a, c) => 1', '(...a,) => 1', '(c, ...a,) => 1', '((c)) => 1', '(c, (d)) => 1', '([(c)]) => 1', '({ a: (c) }) => 1', '([c]) => 1 = 2',
  'async (await) => 1', 'async ([await]) => 1', 'async ({ await }) => 1',
  '(c.d) => 1', '(c + d) => 1', '(1) => 1', '(c, 1) => 1', '(c = 1, d.e) => 1', '(...c.d) => 1', 'async (c.d) => 1', 'async (...a, c) => 1',
  '({ m({ a: o.x }) {} })', '({ set s([o.x]) {} })', 'class K { m([o.x]) {} }', 'class K { constructor({ a: o.x }) {} }', 'function* g([o.x]) {}',
  'try {} catch ({ a: o.x }) {}', 'try {} catch ([o.x]) {}', 'try {} catch (o.x) {}', 'try {} catch ([(c)]) {}',
  'for (var { a: o.x } of []);', 'for (let [o.x] of []);', 'for (const { a: o.x } in {});', 'for (var [o.x] = [1]; false;);', 'for (let o.x of []);',
  'var o.x = 1',
  'class K { #p; static m(o) { var { a: o.#p } = { a: 1 } } }', 'class K { #p; static m(o) { let [o.#p] = [1] } }',
  'class K { #p; static m(o, [o.#p]) {} }', 'class K { #p; static m(o) { try {} catch ({ a: o.#p }) {} } }',
  'class K { #p; static m(o) { return ([o.#p]) => 1 } }', 'class K { #p; static m(o) { return (#p) in o } }', 'class K { #p; static m(o) { return (#p) } }',
  '(...b)', '(b, ...c)', '(...b) + 1', '...b', 'b = ...c', 'o.f((...b))', 'var c = (...b)',
]);

accepts(['o', 'b'], [
  'var { a: c } = { a: 1 }', 'let [c, d = 2, ...e] = [1]', 'const { c, d: { e = 1 } = {}, ...f } = {}', 'var [, c, , d] = []', 'var [c, d,] = []',
  'var { a: [c, { d }], ...e } = { a: [1, {}] }', 'var [c = (o.x), d = o?.y] = []', "var { 'a': c, 1: d, [b]: e } = {}",
  'function f({ a: c }, [d] = [], ...[e, f]) {}', 'function f(c, ...{ length }) {}', 'function f(c,) {}',
  '({ a: c }) => 1', '([c], { d } = {}, ...e) => 1', '(c, d,) => 1', '(c = (b)) => 1', '([c = (b), d = o.x]) => 1', 'async ({ a: c }, [d]) => 1', 'async (c, ...d) => 1',
  '(async (c = 1, [d] = []) => 1)', 'c => 1', 'async c => 1', '() => 1', 'async () => 1', '(undefined) => 1',
  'try {} catch ({ a: c }) {}', 'try {} catch ([c, ...d]) {}', 'try {} catch (c) {}', 'try {} catch {}',
  'for (var { a: c } of []);', 'for (let [c] of []);', 'for (const { a: c } in {});', 'for (var [c] = [1]; false;);', 'for (var c in {});',
  'o.f(...b)', 'o.f(b, ...b,)', 'o?.f(...b)', 'o?.(...b)', 'new o.f(...b)', '[...b, ...b]', '({ ...b, ...o })', 'async(...b)', 'async(b, ...b,)',
  '(b)', '(b, o)', '((b), (o))', '(b = 1)', '(b = 1, o.x)', '([b] = [1])', '({ b } = o)', '(o.f)(b)', '(function () {})()', '(class {})',
]);

rejects(['o', 'F', 't'], [
  'o?.t`a`', 'o?.[1]`a`', 'o?.()`a`', 'o?.a.b`c`', 'o?.a\n`c`', 'o?.a(1)`c`', 'o?.a[0]`c`', 'o.a?.b.c`d`', 'o?.a?.b`c`',
  'new o?.a()', 'new o?.a', 'new o.a?.b()', 'new o?.[0]', 'new o?.()', 'new F?.a',
]);

accepts(['o', 'F', 't'], [
  '(o?.t)`a`', '(o?.a.b)`c`', 'o?.a(`x`)', 'o?.[`x`]', 'o?.a(t`x`)', 'o.t`a`', 'o.a.b`c`?.d', 't`a`?.b',
  'new F()?.a', 'new (o?.a)()', 'new (o?.a)', 'new F().a?.b',
]);

{
  const run = (src, ...args) => new Function('o', 'q', src)(...args);
  assert.strictEqual(run('(o?.a).b = 5; return o.a.b', { a: { b: 1 } }), 5);
  assert.strictEqual(run('[(o?.a).b] = [6]; return o.a.b', { a: { b: 1 } }), 6);
  assert.strictEqual(run('(o?.a).b++; return o.a.b', { a: { b: 1 } }), 2);
  assert.strictEqual(run('for ((o?.a).b of [7]); return o.a.b', { a: { b: 1 } }), 7);
  assert.throws(() => run('(o?.a).b = 5', {}), TypeError);
  assert.deepStrictEqual(run('var [c, ...d] = o; return [c, d]', [1, 2, 3]), [1, [2, 3]]);
  assert.deepStrictEqual(run('return (([c, { d = 4 }], ...e) => [c, d, e])(o, 5, 6)', [1, {}]), [1, 4, [5, 6]]);
  assert.deepStrictEqual(run('return (async (c, ...d) => [c, d])(...o)', [1, 2]) instanceof Promise, true);
  assert.deepStrictEqual(run('return Math.max(...o, ...q)', [1, 5], [3]), 5);
  assert.deepStrictEqual(run('return o?.f(...q)', { f: (...a) => a }, [1, 2]), [1, 2]);
  assert.deepStrictEqual(run('return o?.(...q)', (...a) => a, [1, 2]), [1, 2]);
  assert.deepStrictEqual(run('return (0, o)?.(...q, 3)', (...a) => a, [1, 2]), [1, 2, 3]);
}

console.log('ok');
