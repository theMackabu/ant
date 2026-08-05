// closures created inside a direct-eval scope carry SV_CALL_HAS_EVAL_ENV;
// `new` on them must not be mistaken for a native ctor (JS_NATIVE_CTOR
// once shared the same call_flags bit and misrouted them through
// sv_call_native: "function is not a function")

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

function outer() {
  var local = 40;
  return eval("var q = 2; function C(){ this.x = local + q; }; C");
}
const C = outer();
const c = new C();
assert(c.x === 42, 'ctor declared in eval scope: got ' + c.x);
assert(c instanceof C, 'instanceof through eval-scope ctor');

function outer2() {
  return eval("var w = 5; (function D(){ this.y = w; })");
}
const D = outer2();
assert(new D().y === 5, 'ctor expression in eval scope');

class E {
  constructor() { this.z = eval("var v = 3; v"); }
}
assert(new E().z === 3, 'class ctor running direct eval');

// native ctors still allocate their own receivers
const blob = new Blob(['hi']);
assert(blob.size === 2 && blob instanceof Blob, 'native ctor receiver');

console.log('ctor-from-eval-scope tests passed');
