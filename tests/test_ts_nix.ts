import assert from 'node:assert';
import { execFileSync } from 'node:child_process';
import { Expr, AttrSet, BinaryCache, DevShell, EachSystem, Flake, Input, Let, NixFunction, Outputs, Package, Shell, call, ifElse, lambda, letIn, nix, path, ref, render, select, str } from '../packages/ts-nix/index.ts';

const literal = { 'a.b': '${notCode}', quote: '"\\\n\r\t', unicode: '🐜', values: [null, true, false, -42, 1.25, 1e-7] };
assert.match(render(literal), /\\\$\{notCode\}/);
assert.throws(() => render(undefined as any), /Unsupported/);
assert.throws(() => render(NaN), /finite/);
assert.throws(() => render(2 ** 53), /safe/);
assert.throws(() => render(new Date() as any), /plain/);
assert.throws(() => render('\0'), /NUL/);
const cycle: any = {}; cycle.self = cycle;
assert.throws(() => render(cycle), /Cyclic/);
assert.throws(() => render([,] as any), /Unsupported/);
assert.throws(() => ref('foo.bar'), /binding/);
assert.throws(() => ref('if'), /binding/);
assert.throws(() => path('./foo ${bad}'), /path/);
assert.throws(() => lambda(['x', 'x'], null), /Duplicate/);
assert.throws(() => select(null), /attribute/);
assert.throws(() => call(ref('f')), /argument/);
assert.strictEqual(render(path('../file.nix')), '../file.nix\n');
assert.strictEqual(render({ a: {}, b: {} }), '{\n  a = {};\n  b = {};\n}\n');

const baseAttrs = new AttrSet({ original: true });
const baseFlake = new Flake('test');
const nixpkgs = new Input('nixpkgs', 'github:NixOS/nixpkgs');
const utils = new Input('utils', 'github:numtide/flake-utils');
const overlay = new Input('overlay', 'github:example/overlay').follows('nixpkgs', nixpkgs);
const project = baseFlake.inputs(nixpkgs, utils, overlay).cache(new BinaryCache('https://cache.example', 'key'));
const identity = () => new NixFunction('x', x => x);
assert.strictEqual(identity().render(), identity().render());
assert.throws(() => new NixFunction(['x', 'x'], () => null), /Duplicate/);
assert.throws(() => new NixFunction('__tsnix_0_x', x => x), /reserved/);
assert.throws(() => new Input('self', 'url'), /Reserved/);
assert.throws(() => baseFlake.inputs(nixpkgs, nixpkgs), /Duplicate/);
assert.throws(() => new Outputs(new Package('one', 1).asDefault(), new Package('two', 2).asDefault()), /Duplicate/);
assert.throws(() => new Outputs(new DevShell('dev', 1), new DevShell('dev', 2)), /Duplicate/);
let closedScope: any;
new Let(scope => { closedScope = scope; return scope.bind('value', 1); });
assert.throws(() => closedScope.bind('late', 2), /closed/);

assert.throws(() => new NixFunction(['x'], ({ x }) => x, { missing: 1 } as any), /Unknown default/);
assert.throws(() => str`bad\0${'value'}`, /NUL/);

assert.strictEqual(new NixFunction(['pkgs'], ({ pkgs }) => pkgs.get('mkShell').call({
  packages: [pkgs.get('hello')],
})).render(), '{ pkgs }:\npkgs.mkShell {\n  packages = [ pkgs.hello ];\n}\n');
assert.strictEqual(ref('f').call(1).call(2).render(), 'f 1 2\n');

// Nix evaluates the result, catching precedence and escaping errors beyond snapshots.
if (process.argv.includes('--nix')) {
  const evaluate = (value: any) => JSON.parse(execFileSync('nix-instantiate', ['--eval', '--strict', '--json', '--expr', render(value)], { encoding: 'utf8' }));
  assert.deepStrictEqual(evaluate(literal), literal);
  assert.deepStrictEqual(evaluate({ nested: { text: nix`"first
second"` } }), { nested: { text: 'first\nsecond' } });
  assert.deepStrictEqual(evaluate([call(lambda('x', nix`x + 1`), 4), ifElse(true, 'yes', 'no')]), [5, 'yes']);
  assert.strictEqual(evaluate(letIn({ x: 7 }, call(lambda(['y'], nix`x + y`), { y: 2 }))), 9);
  assert.strictEqual(evaluate(select({ 'a.b': { if: 8 } }, 'a.b', 'if')), 8);
  assert.strictEqual(evaluate(nix`builtins.stringLength ${'${literal}'}`), 10);
  const sameName = new NixFunction('x', outer => new NixFunction('x', inner => [outer, inner]));
  assert.deepStrictEqual(evaluate(sameName.call(1).call(2)), [1, 2]);
  const patterns = new NixFunction(['x'], outer => new NixFunction(['x'], inner => [outer.x, inner.x]));
  assert.deepStrictEqual(evaluate(patterns.call({ x: 3 }).call({ x: 4 })), [3, 4]);
  assert.deepStrictEqual(evaluate(new Let(scope => {
    const outer = scope.bind('same', 5);
    return new Let(inner => [outer, inner.bind('same', 6)]);
  })), [5, 6]);
  assert.deepStrictEqual(evaluate(baseAttrs), { original: true });
  assert.deepStrictEqual(evaluate(baseAttrs.set('extra', false)), { original: true, extra: false });
  assert.strictEqual(evaluate(baseAttrs.getOr(9, 'missing')), 9);
  assert.strictEqual(evaluate(baseAttrs.getOr(9, 'original')), true);
  assert.strictEqual(evaluate(baseAttrs.set('nil', null).getOr(9, 'nil')), null);
  assert.deepStrictEqual(evaluate(baseAttrs.merge({ original: false })), { original: false });
  assert.deepStrictEqual(evaluate(nix`[1]`.concat([2])), [1, 2]);
  assert.strictEqual(evaluate(nix`1`.equals(1)), true);
  const outputs = new Outputs(new Package('ant', 'binary').asDefault());
  assert.deepStrictEqual(evaluate(outputs), { packages: { ant: 'binary', default: 'binary' } });
  assert.deepStrictEqual(evaluate(outputs.add(new DevShell('dev', 'shell').asDefault())), {
    packages: { ant: 'binary', default: 'binary' }, devShells: { dev: 'shell', default: 'shell' },
  });
  const pkgs = new AttrSet({ mkShell: identity(), meson: 'meson', ninja: 'ninja' });
  const shell = new Shell(pkgs).packages(pkgs.get('meson')).env({ A: 'a' }).hook('first');
  assert.deepStrictEqual(evaluate(shell), { packages: ['meson'], env: { A: 'a' }, shellHook: 'first' });
  assert.deepStrictEqual(evaluate(shell.packages(pkgs.get('ninja')).env({ B: 'b' }).hook('second')), {
    packages: ['meson', 'ninja'], env: { A: 'a', B: 'b' }, shellHook: 'first\nsecond',
  });
  const flake = project.outputs(inputs => new EachSystem(inputs.utils, system => new Outputs(
    new Package('ant', system).asDefault(),
    new DevShell('default', inputs.nixpkgs.get('shell')),
  )));
  assert.deepStrictEqual(evaluate(flake.get('inputs')), {
    nixpkgs: { url: 'github:NixOS/nixpkgs' }, utils: { url: 'github:numtide/flake-utils' },
    overlay: { url: 'github:example/overlay', inputs: { nixpkgs: { follows: 'nixpkgs' } } },
  });
  assert.deepStrictEqual(evaluate(baseFlake.outputs(() => ({})).get('inputs')), {});
  assert.deepStrictEqual(evaluate(flake.get('nixConfig')), {
    'extra-substituters': ['https://cache.example'], 'extra-trusted-public-keys': ['key'],
  });
  assert.deepStrictEqual(evaluate(flake.get('outputs').call({
    self: {}, nixpkgs: { shell: 'dev' }, overlay: {},
    utils: { lib: { eachDefaultSystem: new NixFunction('build', build => build.call('test-system')) } },
  })), { packages: { ant: 'test-system', default: 'test-system' }, devShells: { default: 'dev' } });
  const optional = new NixFunction(['value'], ({ value }) => value, { value: 'fallback' });
  assert.strictEqual(evaluate(optional.call({})), 'fallback');
  assert.strictEqual(evaluate(optional.call({ value: null })), null);
  assert.strictEqual(evaluate(str`prefix ${new AttrSet({ value: 'middle' }).get('value')} suffix`), 'prefix middle suffix');
  assert.strictEqual(evaluate(str`literal \${shell} ${'"\\\n'}`), 'literal ${shell} "\\\n');
  assert.deepStrictEqual(evaluate(new AttrSet().set(str`CARGO_${'TARGET'}_LINKER`, 'clang')), { CARGO_TARGET_LINKER: 'clang' });
  assert.deepStrictEqual(evaluate(new AttrSet({ value: 1 }).set(str`${'value'}`, 2)), { value: 2 });
  const scripts = [
    'first\nsecond\n', '\nfirst\n', '\n\nfirst\n',
    '  entirely indented\n  second\n', 'first\n  indented\n',
    "''\n${literal}\n$${literal}\n\\\n\t\r\n",
  ];
  assert.deepStrictEqual(evaluate(scripts), scripts);
  assert.strictEqual(evaluate(str`before ${new Expr('"first\nsecond"')} after
`), 'before first\nsecond after\n');
  assert.deepStrictEqual(evaluate(new NixFunction('builtins', arg => [
    arg, ref('builtins').get('stringLength').call('abc'),
  ]).call(4)), [4, 3]);
  assert.deepStrictEqual(evaluate(nix`[1]`.concat(nix`[2]`.concat([3]))), [1, 2, 3]);
  assert.deepStrictEqual(evaluate(new AttrSet({ x: 1 }).merge({ x: 2 }).merge({ y: 3 })), { x: 2, y: 3 });
  const empty = Object.create(null); empty.value = 'ok';
  assert.deepStrictEqual(evaluate(empty), { value: 'ok' });
}
console.log('TS → Nix tests passed' + (process.argv.includes('--nix') ? ' (including Nix evaluation)' : ''));
