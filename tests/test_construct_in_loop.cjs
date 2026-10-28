const assert = require('node:assert');

const out = [];
const log = (...values) => out.push(values.map(v => typeof v === 'string' ? v : JSON.stringify(v)).join(' '));

function Plain(x) { this.x = x; }
function ReturnsObject(x) { this.x = x; return { y: x * 2 }; }
function ReturnsPrimitive(x) { this.x = x; return 42; }
function ReturnsNull(x) { this.x = x; return null; }
function ReturnsFunction(x) { this.x = x; return function inner() { return x; }; }
function ReturnsUndefinedExplicit(x) { this.x = x; return undefined; }
function ReturnsThisLater(x) { this.x = x; if (x > 1) return this; this.small = true; }
function FinallyReturn(x) { try { this.x = x; return 7; } finally { this.f = 1; } }
function FinallyOverride(x) { try { return { from: 'try' }; } finally { if (x) return { from: 'finally' }; } }
function FinallyPrimitive(x) { try { this.x = x; } finally { return 'p'; } }
function NewTarget() { this.nt = new.target === NewTarget; this.called = typeof new.target; }
function Thrower(x) { this.x = x; if (x === 3) throw new Error('boom ' + x); }
function ArgsUser() { this.n = arguments.length; this.sum = Array.prototype.reduce.call(arguments, (a, b) => a + b, 0); }
function Defaults(a, b = a + 1, ...rest) { this.a = a; this.b = b; this.rest = rest; }
function Chain(n) { this.next = n ? new Chain(n - 1) : null; this.n = n; }
function TailInCtor(x) { this.x = x; return helper(x); }
function helper(x) { return x > 0 ? { tail: x } : x; }
function LoopCtor(n) { let s = 0; for (let i = 0; i < n; i++) s += i; this.s = s; }
function Proto(x) { this.x = x; }
Proto.prototype.get = function () { return this.x; };
function BadProto() { this.ok = true; }
BadProto.prototype = 5;
class Base { constructor(x) { this.x = x; } get double() { return this.x * 2; } }
class Derived extends Base { constructor(x) { super(x + 1); this.y = x; } }
class Fields { a = 1; b = this.a + 1; constructor(c) { this.c = c; } }
class Implicit extends Base {}
function Closure(x) { this.get = () => x + this.k; this.k = 10; }

log('plain', new Plain(1));
log('object', new ReturnsObject(2));
log('primitive', new ReturnsPrimitive(3));
log('null', new ReturnsNull(4));
log('function', typeof new ReturnsFunction(5), new ReturnsFunction(5)());
log('undef', new ReturnsUndefinedExplicit(6));
log('this-later', new ReturnsThisLater(2), new ReturnsThisLater(1));
log('finally', new FinallyReturn(8));
log('finally-override', new FinallyOverride(1), new FinallyOverride(0));
log('finally-primitive', new FinallyPrimitive(9));
log('new-target', new NewTarget());
log('args', new ArgsUser(1, 2, 3, 4));
log('defaults', new Defaults(1), new Defaults(1, 5, 6, 7));
log('tail', new TailInCtor(2), new TailInCtor(0));
log('loop', new LoopCtor(100000).s);
log('proto', new Proto(11).get(), new Proto(12) instanceof Proto);
log('bad-proto', Object.getPrototypeOf(new BadProto()) === Object.prototype);
log('derived', new Derived(1), new Derived(1).double);
log('fields', new Fields(3));
log('implicit', new Implicit(4));
log('closure', new Closure(5).get());
for (let i = 0; i < 5; i++) {
  try { log('throw', new Thrower(i)); } catch (e) { log('caught', e.message); }
}

let depth = 0;
let c = new Chain(3000);
while (c) { depth++; c = c.next; }
log('chain', depth);

let total = 0;
for (let i = 0; i < 20000; i++) {
  total += new Plain(i).x + new ReturnsObject(i).y + new ReturnsPrimitive(i).x;
  total += new FinallyReturn(i).x + (new ReturnsThisLater(i % 3).x || 0);
}
log('hot', total);

function Unbounded() { new Unbounded(); }
try { new Unbounded(); log('unbounded', 'no error'); } catch (e) { log('unbounded', e.constructor.name); }
log('after-overflow', new Plain('ok'));

const expected = [
  'plain {"x":1}',
  'object {"y":4}',
  'primitive {"x":3}',
  'null {"x":4}',
  'function function 5',
  'undef {"x":6}',
  'this-later {"x":2} {"x":1,"small":true}',
  'finally {"x":8,"f":1}',
  'finally-override {"from":"finally"} {"from":"try"}',
  'finally-primitive {"x":9}',
  'new-target {"nt":true,"called":"function"}',
  'args {"n":4,"sum":10}',
  'defaults {"a":1,"b":2,"rest":[]} {"a":1,"b":5,"rest":[6,7]}',
  'tail {"tail":2} {"x":0}',
  'loop 4999950000',
  'proto 11 true',
  'bad-proto true',
  'derived {"x":2,"y":1} 4',
  'fields {"a":1,"b":2,"c":3}',
  'implicit {"x":4}',
  'closure 15',
  'throw {"x":0}',
  'throw {"x":1}',
  'throw {"x":2}',
  'caught boom 3',
  'throw {"x":4}',
  'chain 3001',
  'hot 999969999',
  'unbounded RangeError',
  'after-overflow {"x":"ok"}',
];
assert.deepStrictEqual(out, expected);
console.log('PASS construct frames');
