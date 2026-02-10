import { test, summary } from './helpers.js';

console.log('Private Class Tests\n');

// Private instance fields
class Counter {
  #count = 0;
  increment() { this.#count++; }
  get value() { return this.#count; }
}
const counter = new Counter();
test('private field initial', counter.value, 0);
counter.increment();
counter.increment();
test('private field after increment', counter.value, 2);

// Private field with constructor
class Box {
  #content;
  constructor(val) { this.#content = val; }
  unwrap() { return this.#content; }
}
test('private field via constructor', new Box(42).unwrap(), 42);
test('private field string', new Box('hello').unwrap(), 'hello');

// Private field does not collide with public
class Dual {
  #x = 'private';
  x = 'public';
  getPrivate() { return this.#x; }
  getPublic() { return this.x; }
}
const dual = new Dual();
test('private vs public field', dual.getPrivate(), 'private');
test('public field unaffected', dual.getPublic(), 'public');

// Private instance methods
class Formatter {
  #prefix;
  constructor(prefix) { this.#prefix = prefix; }
  #format(str) { return this.#prefix + ': ' + str; }
  display(str) { return this.#format(str); }
}
test('private method', new Formatter('LOG').display('test'), 'LOG: test');

// Private method does not collide with public
class Methods {
  #x() { return 'private'; }
  x() { return this.#x(); }
}
test('private method call', new Methods().x(), 'private');

// Private getters and setters
class Temperature {
  #celsius = 0;
  get #c() { return this.#celsius; }
  set #c(val) { this.#celsius = val; }
  get fahrenheit() { return this.#c * 9 / 5 + 32; }
  set fahrenheit(val) { this.#c = (val - 32) * 5 / 9; }
}
const temp = new Temperature();
test('private getter initial', temp.fahrenheit, 32);
temp.fahrenheit = 212;
test('private setter', temp.fahrenheit, 212);

// Private static methods
class MathHelper {
  static #square(n) { return n * n; }
  static calc(n) { return MathHelper.#square(n); }
}
test('private static method', MathHelper.calc(5), 25);

// Private static accessors
class Config {
  static #data = null;
  static get #config() { return Config.#data || 'default'; }
  static set #config(val) { Config.#data = val; }
  static get() { return Config.#config; }
  static set(val) { Config.#config = val; }
}
test('private static getter default', Config.get(), 'default');
Config.set('custom');
test('private static setter', Config.get(), 'custom');

// Private static fields
class IdGen {
  static #next = 1;
  static generate() { return IdGen.#next++; }
}
test('private static field 1', IdGen.generate(), 1);
test('private static field 2', IdGen.generate(), 2);
test('private static field 3', IdGen.generate(), 3);

// Computed instance fields
class Computed {
  ['x'] = 10;
  ['y'] = 20;
}
const comp = new Computed();
test('computed instance field x', comp.x, 10);
test('computed instance field y', comp.y, 20);

// Computed static fields
class ComputedStatic {
  static ['a'] = 100;
  static ['b'] = 200;
}
test('computed static field a', ComputedStatic.a, 100);
test('computed static field b', ComputedStatic.b, 200);

// Static initialization blocks
let staticVal = 0;
class WithStaticBlock {
  static { staticVal = 42; }
}
test('static init block', staticVal, 42);

let staticSum = 0;
class MultiBlock {
  static { staticSum += 10; }
  static { staticSum += 20; }
}
test('multiple static blocks', staticSum, 30);

// Inheritance with private fields
class Animal {
  #name;
  constructor(name) { this.#name = name; }
  getName() { return this.#name; }
}
class Dog extends Animal {
  #breed;
  constructor(name, breed) {
    super(name);
    this.#breed = breed;
  }
  describe() { return this.getName() + ' (' + this.#breed + ')'; }
}
test('inherited private fields', new Dog('Rex', 'Labrador').describe(), 'Rex (Labrador)');

summary();
