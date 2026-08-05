import { test, summary } from './helpers.js';

console.log('constructors declared in eval scopes\n');

function makeDeclCtor() {
  var local = 40;
  return eval("var q = 2; function C(){ this.x = local + q; }; C");
}
const DeclCtor = makeDeclCtor();
const declInstance = new DeclCtor();
test('function declaration in eval scope constructs', declInstance.x, 42);
test('instanceof through eval-scope ctor', declInstance instanceof DeclCtor, true);
test('prototype linked normally', Object.getPrototypeOf(declInstance), DeclCtor.prototype);

function makeExprCtor() {
  return eval("var w = 5; (function D(a){ this.y = w + a; })");
}
const ExprCtor = makeExprCtor();
test('function expression in eval scope constructs with args', new ExprCtor(3).y, 8);

function makeClassCtor() {
  return eval("var base = 10; class E { constructor(n) { this.z = base + n; } }; E");
}
const ClassCtor = makeClassCtor();
test('class declared in eval scope constructs', new ClassCtor(4).z, 14);

console.log('\neval scope captures\n');

function makeCounter() {
  return eval("var n = 0; (function(){ return ++n; })");
}
const counter = makeCounter();
counter();
test('eval-scope closure keeps its own binding', counter(), 2);

function readerAndCtor() {
  return eval("var shared = 7; [function R(){ return shared; }, function W(){ this.v = shared; }]");
}
const [reader, Writer] = readerAndCtor();
test('call and construct share the eval binding', reader() + new Writer().v, 14);

class DirectEvalInCtor {
  constructor() { this.z = eval("var v = 3; v"); }
}
test('class ctor running direct eval', new DirectEvalInCtor().z, 3);

summary();
