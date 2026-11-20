const assert = require('node:assert');

function nest(depth, inner) {
  return '() => '.repeat(depth) + inner;
}

function unwrap(fn, depth) {
  for (let i = 0; i < depth; i++) fn = fn();
  return fn;
}

const started = Date.now();
let deep;
try {
  deep = new Function('return ' + nest(60000, '1'));
} catch (error) {
  assert.ok(error instanceof RangeError, String(error));
}
assert.ok(Date.now() - started < 10000, `60000 nested arrows took ${Date.now() - started}ms`);
if (deep) assert.strictEqual(typeof deep(), 'function');

for (const depth of [1, 2, 3, 8]) {
  const args = new Function('return ' + nest(depth, 'arguments[0] + arguments.length'));
  assert.strictEqual(unwrap(args('a', 'b'), depth), 'a2', `arguments at depth ${depth}`);

  const evalArgs = new Function('return ' + nest(depth, "eval('arguments[1]')"));
  assert.strictEqual(unwrap(evalArgs('a', 'b'), depth), 'b', `eval arguments at depth ${depth}`);

  const evalVar = new Function('var local = 7; return ' + nest(depth, "eval('var made = local + 1; made')"));
  assert.strictEqual(unwrap(evalVar(), depth), 8, `eval var at depth ${depth}`);

  const evalParam = new Function('return ' + nest(depth - 1, "(p = eval('arguments[0]')) => p"));
  assert.strictEqual(unwrap(evalParam('q'), depth), 'q', `eval in parameter at depth ${depth}`);

  const target = new Function('function F() { return ' + nest(depth, 'new.target') + ' } return [new F, F()]');
  const [constructed, called] = target();
  assert.strictEqual(typeof unwrap(constructed, depth), 'function', `new.target at depth ${depth}`);
  assert.strictEqual(unwrap(called, depth), undefined, `no new.target at depth ${depth}`);

  const evalTarget = new Function('function F() { return ' + nest(depth, "eval('new.target')") + ' } return new F');
  assert.strictEqual(typeof unwrap(evalTarget(), depth), 'function', `eval new.target at depth ${depth}`);

  const gen = new Function('return function* () { const f = ' + nest(depth, "arguments[0]") + '; yield f; }');
  assert.strictEqual(unwrap(gen()('g').next().value, depth), 'g', `generator arguments at depth ${depth}`);

  const sibling = new Function('const plain = ' + nest(depth, '1') + '; return [plain, ' + nest(depth, 'arguments[0]') + ']');
  assert.strictEqual(unwrap(sibling('s')[1], depth), 's', `sibling chains at depth ${depth}`);

  const length = new Function('return ' + nest(depth, 'arguments.length'));
  assert.strictEqual(unwrap(length(1, 2, 3), depth), 3, `arguments.length at depth ${depth}`);
}

for (const depth of [2, 3, 6]) {
  const run = (body, ...args) => new Function('function f() { new.target; ' + body + ' } return f')()(...args);

  assert.strictEqual(unwrap(run('return ' + nest(depth, 'arguments[0] + arguments.length'), 'a', 'b'), depth), 'a2', `arguments after new.target at depth ${depth}`);
  assert.strictEqual(unwrap(run('return ' + nest(depth, "eval('arguments[1]')"), 'a', 'b'), depth), 'b', `eval arguments after new.target at depth ${depth}`);
  assert.strictEqual(unwrap(run('var local = 4; return ' + nest(depth, "eval('local + arguments.length')"), 1, 2), depth), 6, `eval local after new.target at depth ${depth}`);
  assert.strictEqual(unwrap(run('return ' + nest(depth, "eval('var made = 3; made')")), depth), 3, `eval var after new.target at depth ${depth}`);
  assert.strictEqual(unwrap(run('return ' + nest(depth, 'arguments.length'), 1, 2, 3), depth), 3, `arguments.length after new.target at depth ${depth}`);

  const hidden = new Function('return () => { let hidden = 5; return ' + nest(depth, "eval('hidden')") + ' }')();
  assert.strictEqual(unwrap(hidden(), depth), 5, `eval reads an outer arrow local at depth ${depth}`);

  const shadow = new Function('return () => { var shadowed = 1; return ' + nest(depth, "eval('var shadowed = 2; shadowed')") + ' }')();
  assert.strictEqual(unwrap(shadow(), depth), 2, `eval declares in the inner arrow at depth ${depth}`);

  const Derived = new Function(
    'class Base { constructor() { this.target = new.target } }' +
    'class Derived extends Base { constructor() { const call = ' + nest(depth, 'super()') + '; ' + 'call' + '()'.repeat(depth) + ' } }' +
    'return Derived'
  )();
  assert.strictEqual(new Derived().target, Derived, `super() in arrows at depth ${depth}`);

  const viaCall = new Function('function F() { return ' + nest(depth, 'new.target') + ' } return F')();
  assert.strictEqual(unwrap(new viaCall(), depth), viaCall, `new.target value at depth ${depth}`);
  assert.strictEqual(unwrap(Reflect.construct(viaCall, [], Object), depth), Object, `explicit new.target at depth ${depth}`);
}

const manyProps = Array.from({ length: 40 }, (_, i) => `this.p${i} = ${i};`).join(' ');

for (const depth of [2, 3, 6]) {
  const gen = (body, ...args) => new Function('function* g() { new.target; ' + body + ' } return g')()(...args).next().value;

  assert.strictEqual(unwrap(gen('yield ' + nest(depth, 'arguments[0] + arguments.length'), 'a', 'b'), depth), 'a2', `generator arguments after new.target at depth ${depth}`);
  assert.strictEqual(unwrap(gen('yield ' + nest(depth, "eval('arguments[1]')"), 'a', 'b'), depth), 'b', `generator eval arguments after new.target at depth ${depth}`);
  assert.strictEqual(unwrap(gen('var local = 4; yield ' + nest(depth, "eval('local + arguments.length')"), 1, 2), depth), 6, `generator eval local after new.target at depth ${depth}`);
  assert.strictEqual(unwrap(gen('yield ' + nest(depth, "eval('var made = 3; made')")), depth), 3, `generator eval var after new.target at depth ${depth}`);

  const wide = (body) => new Function('function Wide() { ' + manyProps + ' ' + body + ' } return Wide')();

  const WideTarget = wide('this.chain = ' + nest(depth, 'new.target') + ';');
  assert.strictEqual(unwrap(new WideTarget().chain, depth), WideTarget, `new.target after many this stores at depth ${depth}`);

  const WideEvalTarget = wide('this.chain = ' + nest(depth, "eval('new.target')") + ';');
  assert.strictEqual(unwrap(new WideEvalTarget().chain, depth), WideEvalTarget, `eval new.target after many this stores at depth ${depth}`);

  const WideArgs = wide('new.target; this.chain = ' + nest(depth, 'arguments[0]') + ';');
  assert.strictEqual(unwrap(new WideArgs('w').chain, depth), 'w', `arguments after many this stores at depth ${depth}`);

  const WideDynamicEval = wide('new.target; const source = "arguments[0]"; this.chain = ' + nest(depth, 'eval(source)') + ';');
  assert.strictEqual(unwrap(new WideDynamicEval('d').chain, depth), 'd', `computed eval source after many this stores at depth ${depth}`);
  assert.strictEqual(unwrap(gen('const source = "arguments[1]"; yield ' + nest(depth, 'eval(source)'), 'a', 'b'), depth), 'b', `generator computed eval source at depth ${depth}`);

  const WideEvalArgs = wide('new.target; this.chain = ' + nest(depth, "eval('arguments[0]')") + ';');
  assert.strictEqual(unwrap(new WideEvalArgs('e').chain, depth), 'e', `eval arguments after many this stores at depth ${depth}`);
}

const mixed = new Function('return () => [() => 1, () => () => arguments[0], function () { return arguments[0] }]');
const parts = mixed('outer')();
assert.strictEqual(parts[1]()(), 'outer');
assert.strictEqual(parts[2]('inner'), 'inner');

console.log('ok');
