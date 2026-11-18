const assert = require('node:assert');

const calls = [];
function tag(strings, ...values) {
  calls.push([new.target ? 'new' : 'call', strings.raw.join('|'), values]);
  return function Made(arg) {
    this.arg = arguments.length === 0 ? 'none' : arg && arg.raw ? 'tpl:' + arg.raw.join('|') : arg;
  };
}
const ns = { tag, deep: { tag } };
const take = () => calls.splice(0);

let made = new tag`x`;
assert.strictEqual(made.constructor.name, 'Made');
assert.strictEqual(made.arg, 'none');
assert.deepStrictEqual(take(), [['call', 'x', []]]);

made = new tag`x`(1);
assert.strictEqual(made.arg, 1);
assert.deepStrictEqual(take(), [['call', 'x', []]]);

made = new ns.tag`a${1}b${2}c`(7);
assert.strictEqual(made.arg, 7);
assert.deepStrictEqual(take(), [['call', 'a|b|c', [1, 2]]]);

made = new ns.deep['tag']`q`;
assert.strictEqual(made.arg, 'none');
assert.deepStrictEqual(take(), [['call', 'q', []]]);

made = new (tag)`p`;
assert.strictEqual(made.constructor.name, 'Made');
assert.deepStrictEqual(take(), [['call', 'p', []]]);

function chain(strings) {
  calls.push(['chain', strings.raw[0]]);
  return chain2;
}
function chain2(strings) {
  calls.push(['chain2', strings.raw[0]]);
  return function Last() { this.last = true; };
}
made = new chain`x``y`;
assert.strictEqual(made.last, true);
assert.deepStrictEqual(take(), [['chain', 'x'], ['chain2', 'y']]);

function outerTag(strings) {
  calls.push(['outer', strings.raw[0]]);
  return function Inner() {
    return function Wrapped() { this.wrapped = true; };
  };
}
made = new new outerTag`a`;
assert.strictEqual(made.constructor.name, 'Wrapped');
assert.strictEqual(made.wrapped, true);
assert.deepStrictEqual(take(), [['outer', 'a']]);

function innerTag(strings) {
  calls.push(['inner', strings.raw[0]]);
  return function First() {
    return function second(parts) {
      calls.push(['second', parts.raw[0]]);
      return function Final() { this.final = true; };
    };
  };
}
made = new new innerTag`a`()`b`;
assert.strictEqual(made.final, true);
assert.deepStrictEqual(take(), [['inner', 'a'], ['second', 'b']]);

function Maker() {
  return function called(strings) { return 'called:' + strings.raw[0]; };
}
assert.strictEqual(new Maker()`x`, 'called:x');
assert.strictEqual(new Maker`ignored`({ raw: ['w'] }).constructor.name, 'called');

function Holder() {
  return { member: function Member() { this.member = true; }, list: [function Item() { this.item = true; }] };
}
assert.strictEqual((new Holder`x`.member).member, true);
function ListTag() { return [function Item() { this.item = true; }]; }
assert.strictEqual((new ListTag`x`[0]).item, true);

assert.throws(() => new tag`x``y`, TypeError);
take();

const rejected = [
  'o?.t`a`',
  'o?.[1]`a`',
  'o?.()`a`',
  'o?.a.b`c`',
  'o?.a\n`c`',
  'o?.a(1)`c`',
  'o?.a[0]`c`',
  'o.a?.b.c`d`',
  'o?.a?.b`c`',
  'o?.#x`a`',
  'new o?.a()',
  'new o?.a',
  'new F`x`?.a',
  'new F`x',
];
for (const source of rejected) assert.throws(() => new Function(source), SyntaxError, source);

const accepted = [
  '(o?.t)`a`',
  '(o?.a.b)`c`',
  'o?.a(`x`)',
  'o?.[`x`]',
  'o?.a(t`x`)',
  'new F`x`',
  'new F`x`(1)',
  'new a.b`x`(1)',
  'new F`x``y`',
  'new (F)`x`',
  'new new F`a``b`',
  'new F()`x`',
  'new F()?.a',
];
for (const source of accepted) assert.doesNotThrow(() => new Function(source), source);

const o = { t: (strings) => 'tagged:' + strings.raw[0] };
assert.strictEqual((o?.t)`a`, 'tagged:a');
assert.strictEqual(o?.t({ raw: [`plain`] }), 'tagged:plain');
const missing = undefined;
assert.throws(() => (missing?.t)`a`, TypeError);
assert.throws(() => new Function('var o = { F(s) { return this === o } }; return o?.F`x`'), SyntaxError);

console.log('ok');
