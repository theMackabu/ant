const assert = require('node:assert');

const list = (n, f, sep = ',') => {
  const parts = new Array(n);
  for (let i = 0; i < n; i++) parts[i] = f(i);
  return parts.join(sep);
};

const outcome = (make) => {
  try {
    return { value: make() };
  } catch (error) {
    return { error: error.name };
  }
};

const worksOrSyntaxError = (label, make, expected) => {
  const result = outcome(make);
  if (result.error) assert.strictEqual(result.error, 'SyntaxError', label);
  else assert.deepStrictEqual(result.value, expected, label);
};

const count = function () { return arguments.length; };
const Counter = function () { this.n = arguments.length; };

for (const n of [65536, 65537, 70000]) {
  const args = list(n, (i) => i);
  const tooMany = (label, source, ...values) => assert.throws(
    () => new Function('f', 'o', 'F', source)(count, ...values), SyntaxError, `${label} ${n}`);
  tooMany('call', `return f(${args})`);
  tooMany('method call', `return o.f(${args})`, { f: count });
  tooMany('new', `return new F(${args}).n`, null, Counter);
  tooMany('optional call', `return f?.(${args})`);
  tooMany('eval call', `return eval(${args})`);
  tooMany('includes', `return o.includes(${args})`, [0]);
  tooMany('charCodeAt', `return o.charCodeAt(${args})`, 'a');
  tooMany('indexOf', `return o.indexOf(${args})`, 'a');
  tooMany('tail call', `return (function () { return f(${args}); })()`);
  tooMany('super call', `class A { constructor() { this.n = arguments.length; } }
    class B extends A { constructor() { super(${args}); } } return new B().n`);
}

for (const n of [65535, 65536, 70000]) {
  const substitutions = list(n, (i) => '${' + i + '}', '');
  assert.throws(
    () => new Function('tag', 'return tag`' + substitutions + '`')((strings) => strings.length),
    RangeError, `tagged template ${n}`);
  assert.throws(
    () => new Function('o', 'return o.tag`' + substitutions + '`')({ tag: (strings) => strings.length }),
    RangeError, `tagged method template ${n}`);
}

for (const n of [65534, 65535, 65536, 65537, 70000]) {
  const numbers = list(n, (i) => i);
  let a = new Function(`return [${numbers}]`)();
  assert.deepStrictEqual([a.length, a[0], a[65535 % n], a[n - 1]], [n, 0, 65535 % n, n - 1], `array ${n}`);
  a = new Function('x', `return [${list(n, (i) => 'x+' + i)}]`)(1);
  assert.deepStrictEqual([a.length, a[0], a[n - 1]], [n, 1, n], `dynamic array ${n}`);
  a = new Function('x', `return [...x,${numbers}]`)([-1]);
  assert.deepStrictEqual([a.length, a[0], a[1], a[n]], [n + 1, -1, 0, n - 1], `spread array ${n}`);
  a = new Function('x', `return [${list(n, (i) => 'x+"s' + i + '"')}]`)('');
  assert.deepStrictEqual([a.length, a[0], a[n - 1]], [n, 's0', 's' + (n - 1)], `strings ${n}`);
  a = new Function('o', `return [${list(n, (i) => 'o.a' + i)}]`)({ a0: 'first', ['a' + (n - 1)]: 'last' });
  assert.deepStrictEqual([a.length, a[0], a[1], a[n - 1]], [n, 'first', undefined, 'last'], `atoms ${n}`);
  a = new Function('o', `return [${list(n, () => 'o.a')}]`)({ a: 4 });
  assert.deepStrictEqual([a.length, a[0], a[n - 1]], [n, 4, 4], `property sites ${n}`);
  a = new Function(`return [${list(n, (i) => '()=>' + i)}]`)();
  assert.deepStrictEqual([a.length, a[0](), a[n - 1]()], [n, 0, n - 1], `closures ${n}`);

  let o = new Function(`return {${list(n, (i) => 'k' + i + ':' + i)}}`)();
  assert.deepStrictEqual([Object.keys(o).length, o.k0, o['k' + (n - 1)]], [n, 0, n - 1], `object ${n}`);
  o = new Function('x', `return {${list(n, (i) => 'k' + i + ':x+' + i)}}`)(1);
  assert.deepStrictEqual([Object.keys(o).length, o.k0, o['k' + (n - 1)]], [n, 1, n], `dynamic object ${n}`);

  assert.strictEqual(new Function('return `' + list(n, (i) => '${' + (i % 10) + '}', '') + '`')().length, n, `template ${n}`);
  assert.strictEqual(new Function('x', `switch(x){${list(n, (i) => `case ${i}: return ${i + 1};`, '')}}`)(n - 1), n, `switch ${n}`);
  assert.strictEqual(new Function('x', `var r=0;${list(n, () => 'if(x)r++;', '')}return r`)(1), n, `jumps ${n}`);
  assert.strictEqual(new Function(`return (${numbers})`)(), n - 1, `sequence ${n}`);
  assert.strictEqual(new Function(`${list(n, (i) => '{let b' + i + '=1;}', '')}return 1`)(), 1, `blocks ${n}`);
  assert.strictEqual(new Function(`var f;${list(n, (i) => '{let b=' + i + ';f=()=>b}', '')}return f()`)(), n - 1, `block closures ${n}`);
  assert.strictEqual(new Function(`class A{${list(n, (i) => 'f' + i + '=' + i, ';')}} return new A().f${n - 1}`)(), n - 1, `class fields ${n}`);

  const decl = list(n, (i) => 'v' + i + '=' + i);
  const first = `[v0, v1, v${n - 1}]`;
  worksOrSyntaxError(`var ${n}`, () => new Function(`var ${decl}; return ${first}`)(), [0, 1, n - 1]);
  worksOrSyntaxError(`let ${n}`, () => new Function(`let ${decl}; return ${first}`)(), [0, 1, n - 1]);
  worksOrSyntaxError(`captured ${n}`, () => new Function(`let ${decl}; return (() => ${first})()`)(), [0, 1, n - 1]);
  worksOrSyntaxError(
    `array pattern ${n}`,
    () => new Function('a', `var [${list(n, (i) => 'v' + i)}] = a; return ${first}`)(Array.from({ length: n }, (_, i) => i)),
    [0, 1, n - 1]);
  worksOrSyntaxError(
    `object pattern ${n}`,
    () => new Function('o', `var {${list(n, (i) => 'v' + i)}} = o; return ${first}`)({ v0: 0, v1: 1, ['v' + (n - 1)]: n - 1 }),
    [0, 1, n - 1]);
  worksOrSyntaxError(
    `functions ${n}`,
    () => new Function(`${list(n, (i) => 'function f' + i + '(){return ' + i + '}', ';')}; return [f0(), f1(), f${n - 1}()]`)(),
    [0, 1, n - 1]);
  const params = outcome(() => new Function(list(n, (i) => 'p' + i), `return [p0, p1, p${n - 1}]`)(7, 8));
  if (params.error) assert.strictEqual(params.error, 'SyntaxError', `params ${n}`);
  else assert.deepStrictEqual(params.value, [7, 8, undefined], `params ${n}`);
}

{
  const half = 33000;
  const outer = list(half, (i) => 'a' + i + '=' + i);
  const inner = list(half, (i) => 'b' + i + '=' + (i + half));
  const uses = list(half, (i) => 'a' + i + ',b' + i);
  worksOrSyntaxError(
    'captures across two functions',
    () => {
      const values = new Function(`let ${outer}; return (function () { let ${inner}; return (() => [${uses}])(); })()`)();
      return [values.length, values[0], values[1], values[2 * half - 2], values[2 * half - 1]];
    },
    [2 * half, 0, half, half - 1, 2 * half - 1]);
}

for (const n of [255, 256, 257, 511, 512, 513]) {
  const f = new Function(`var ${list(n, (i) => 'v' + i + '=' + i)}; var s = 0; v0 += 5; s += 2;
    for (var q = 0; q < 3; q++) s += q; for (let z = 0; z < 3; z++) s += z; return [v0, s, q, v${n - 1}]`);
  assert.deepStrictEqual(f(), [5, 8, 3, n - 1], `byte slot operands ${n}`);
}

console.log('ok');
