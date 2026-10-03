// Code compiled by eval and the Function constructors is freed once nothing
// can run it again. Everything that escapes from such code must survive the
// collections that free its neighbours: literals, closures, suspended
// generators, classes, errors, direct-eval environments and cached template
// objects. The churn loop also checks that memory stays bounded (the harness
// caps this test's RSS).
const assert = require('node:assert');

const tag = (strings) => strings;
globalThis.tag = tag;

// compiles and drops unique code with object literals, tagged templates,
// BigInt and string literals and inner closures, enough to force majors
function churn(rounds) {
  let sum = 0;
  for (let i = 0; i < rounds; i++) {
    const src = `
      const o = { a: ${i}, b: 'churn-${i}', c: [1, 2, ${i}] };
      const big = ${i}n * 1234567890123456789n;
      function inner(x) { return x + o.a; }
      return inner(1) + tag\`c${i}\`.raw.length + Number(big % 7n) + o.b.length;
    `;
    sum += i % 2 ? new Function(src)() : eval(`(function(){${src}})()`);
  }
  return sum;
}

// values created by dynamic code before the churn
const kept = {
  str: new Function("return 'escaped-literal-" + 12345 + "'")(),
  big: eval('987654321987654321987654321n'),
  key: eval("({ ['computed-key-7']: 1 })"),
  closure: eval(`(function () {
    const secret = 'closure-secret';
    return (n) => secret + ':' + n;
  })()`),
  gen: new Function(`
    return (function* () {
      const label = 'gen-literal';
      yield label + 1;
      yield label + 2;
    })();
  `)(),
  Klass: new Function(`
    return class Point {
      constructor(x) { this.x = x; }
      describe() { return 'point-' + this.x; }
    };
  `)(),
  fn: new Function('a', 'b', "return a + b + '-fn-literal';"),
  thrower: eval(`(function () {
    return function thrower() { throw new TypeError('thrown-from-dead-eval'); };
  })()`),
  directEval: new Function(`
    let captured = 'direct-eval-binding';
    return eval('() => captured + "!"');
  `)(),
  template: new Function('return () => tag`site-literal`;')(),
  // an implicit constructor has no function of its own, only its source text
  DefaultClass: eval('(class CollectedSource {})'),
  BoundClass: eval('(class BoundSource {})').bind(null),
};

// dynamic code hot enough for the JIT, called directly and inlined into a
// caller; compiled code is never freed, so neither is the code it embeds
const hot = new Function('x', "return x + 'hot-literal'.length;");
const add = new Function('a', 'b', 'return a + b;');
function caller(n) {
  let s = 0;
  for (let i = 0; i < n; i++) s = add(s, 1);
  return s;
}
let acc = 0;
for (let i = 0; i < 200000; i++) acc = hot(acc) % 1000003;
assert.strictEqual(caller(200000), 200000);

const templateBefore = kept.template();
const genFirst = kept.gen.next().value;
const fnSourceBefore = kept.fn.toString();

churn(60000);

assert.strictEqual(kept.str, 'escaped-literal-12345');
assert.strictEqual(kept.big, 987654321987654321987654321n);
assert.deepStrictEqual(Object.keys(kept.key), ['computed-key-7']);
assert.strictEqual(kept.closure(3), 'closure-secret:3');

assert.strictEqual(genFirst, 'gen-literal1');
assert.strictEqual(kept.gen.next().value, 'gen-literal2');
assert.strictEqual(kept.gen.next().done, true);

assert.strictEqual(new kept.Klass(4).describe(), 'point-4');
assert.strictEqual(kept.DefaultClass.toString(), 'class CollectedSource {}');
// a bound function has no source text of its own (NativeFunction syntax)
assert.strictEqual(kept.BoundClass.toString(), 'function () { [native code] }');
assert.ok(new kept.BoundClass() instanceof kept.BoundClass);
assert.match(kept.Klass.toString(), /^class Point \{/);

assert.strictEqual(kept.fn(1, 2), '3-fn-literal');
assert.strictEqual(kept.fn.toString(), fnSourceBefore);
assert.match(kept.fn.toString(), /-fn-literal/);

assert.throws(kept.thrower, (err) => {
  assert.ok(err instanceof TypeError);
  assert.strictEqual(err.message, 'thrown-from-dead-eval');
  assert.match(String(err.stack), /thrown-from-dead-eval/);
  return true;
});

assert.strictEqual(kept.directEval(), 'direct-eval-binding!');

// a tagged template's object is cached per call site for as long as the site
// can run, and each compiled copy of the same source gets its own site
const templateAfter = kept.template();
assert.strictEqual(templateAfter, templateBefore);
assert.deepStrictEqual([...templateAfter.raw], ['site-literal']);
const twin = new Function('return () => tag`site-literal`;')();
assert.notStrictEqual(twin(), templateBefore);
assert.strictEqual(twin(), twin());

assert.strictEqual(hot(1), 12);
assert.strictEqual(caller(10), 10);

// a default class defined amid dead code shares its memory block only with
// dead units, so the block is recycled unless the class keeps its source alive
function churnFunctions(rounds) {
  let sum = 0;
  for (let i = 0; i < rounds; i++)
    sum += new Function(`const o = { a: ${i}, b: 'f-${i}' }; return o.a + o.b.length;`)();
  return sum;
}
churnFunctions(5000);
const IsolatedDefault = eval('(class IsolatedDefault {})');
churnFunctions(40000);
assert.strictEqual(IsolatedDefault.toString(), 'class IsolatedDefault {}');

// call feedback is weak: each function records the previous one as its call
// target, and that must not keep the whole chain alive (about 3 MiB per 1,000
// functions here if it did, well past this test's RSS cap)
{
  const source = 'if (fn) fn(); return 1; /*' + 'x'.repeat(1000) + '*/';
  let previous;
  for (let i = 0; i < 40000; i++) {
    const next = new Function('fn', source);
    for (let j = 0; j < 3; j++) next(previous);
    previous = next;
  }
  assert.strictEqual(previous(), 1);
}

// a second churn after using everything once more
churn(30000);
assert.strictEqual(kept.closure(5), 'closure-secret:5');
assert.strictEqual(kept.template(), templateBefore);
assert.strictEqual(kept.str.length, 'escaped-literal-12345'.length);
assert.strictEqual(kept.DefaultClass.toString(), 'class CollectedSource {}');

// polymorphic read and store sites keep case blocks with shape references;
// they go with the unit (the harness caps this test's RSS)
{
  const make = () => [{ a: 1 }, { b: 1, a: 2 }, { c: 1, a: 3 }, { d: 1, a: 4 }];
  let sum = 0;
  for (let i = 0; i < 60000; i++) {
    const read = new Function('o', 'return o.a + ' + i);
    const store = new Function('o', 'o.a = ' + i + '; o.z = 1; return o.a;');
    for (const o of make()) sum += read(o) + store(o);
  }
  assert.ok(sum > 0);
}

console.log('PASS dynamic code is reclaimed and what escapes from it stays valid');
