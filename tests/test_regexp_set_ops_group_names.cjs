const assert = require('node:assert');

function matches(re, inputs) {
  return inputs.map(s => (re.test(s) ? 1 : 0)).join('');
}

const letters = ['a', 'b', 'e', 'z', 'B', 'é', '1', '_', '&', '-'];

assert.strictEqual(matches(/^[\p{L}--[a--z]]$/v, letters), '0111110000');
assert.strictEqual(matches(/^[\p{L}--[a-z]]$/v, letters), '0000110000');
assert.strictEqual(matches(/^[[a-z]--[b-y]--[z]]$/v, letters), '1000000000');
assert.strictEqual(matches(/^[[a-z]&&[^aeiou]&&[a-c]]$/v, letters), '0100000000');
assert.strictEqual(matches(/^[[[a-z]--[b]]&&[a-e]]$/v, letters), '1010000000');
assert.strictEqual(matches(/^[^[a-z]--[b]]$/v, letters), '0100111111');
assert.strictEqual(matches(/^[^[a-z]&&[aeiou]]$/v, letters), '0101111111');
assert.strictEqual(matches(/^[\w--[\d_]]$/v, letters), '1111100000');
assert.strictEqual(matches(/^[\&\&]$/v, letters), '0000000010');
assert.strictEqual(matches(/^[\w--\d]$/v, letters), '1111100100');
assert.strictEqual(matches(/^[[]a]$/v, ['a', '[', ']']), '100');
assert.strictEqual(matches(/^[[^]--a]$/v, ['a', 'b']), '01');
assert.strictEqual(matches(/^[[:alpha:]]$/v, ['a', ':', 'b']), '110');
assert.strictEqual(matches(/^[[:alpha:]]$/, ['a', 'a]', ':]']), '011');
assert.strictEqual(matches(/^[a[^]$/, ['a', '[', '^', 'x']), '1110');
assert.strictEqual(matches(/^[[].$/, ['[\n', '[x']), '01');

function groups(re, input) {
  const m = re.exec(input);
  return m && { ...m.groups };
}

assert.deepStrictEqual(groups(/(?<a$>x)\k<a$>/, 'xx'), { a$: 'x' });
assert.deepStrictEqual(groups(/(?<$a>x)/, 'x'), { $a: 'x' });
assert.deepStrictEqual(groups(/(?<\u0061b>x)\k<ab>/, 'xx'), { ab: 'x' });
assert.deepStrictEqual(groups(/(?<a\u{62}>x)\k<\u0061b>/, 'xx'), { ab: 'x' });
assert.deepStrictEqual(groups(/(?<\ud835\udcd1>x)\k<𝓑>/, 'xx'), { '𝓑': 'x' });
assert.deepStrictEqual(groups(/(?<_\u{1d4d1}>x)/u, 'x'), { '_𝓑': 'x' });
assert.deepStrictEqual(groups(/(?<πb>x)\k<\u03C0b>/, 'xx'), { 'πb': 'x' });
assert.deepStrictEqual(groups(/(?<a$>x)(?<\u0062>y)?/, 'x'), { a$: 'x', b: undefined });
assert.deepStrictEqual(groups(/(?<_0>a)(?<b$>b)(?<_1>c)/, 'abc'), { _0: 'a', b$: 'b', _1: 'c' });
assert.deepStrictEqual(groups(/(?<a$>x)\k<a$>/i, 'xX'), { a$: 'x' });
assert.strictEqual(/(?<a$>x)\k<a$>/i.test('xy'), false);

const long = 'n'.repeat(200);
assert.deepStrictEqual(groups(new RegExp(`(?<${long}>x)\\k<${long}>`), 'xx'), { [long]: 'x' });

assert.deepStrictEqual({ .../(?<a$>x)(?<\u0062>y)?/d.exec('x').indices.groups }, { a$: [0, 1], b: undefined });
assert.strictEqual('xax'.replace(/(?<$>a)/, '[$<$>]'), 'x[a]x');
assert.strictEqual('xax'.replace(/(?<\u0061b>a)/, '[$<ab>]'), 'x[a]x');
assert.strictEqual('xax'.replace(/(?<a$>a)/, (...args) => JSON.stringify(args.at(-1))), 'x{"a$":"a"}x');
assert.deepStrictEqual([...'ab'.matchAll(/(?<a$>.)/g)].map(m => m.groups.a$), ['a', 'b']);
assert.deepStrictEqual(Object.keys(/(?<a$>x)|(?<b>y)/.exec('y').groups), ['a$', 'b']);

console.log('OK');
