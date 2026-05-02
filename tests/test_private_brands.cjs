const assert = (cond, msg) => {
  if (!cond) throw new Error(msg);
};

const assertThrows = (fn, name, msg) => {
  let threw = false;
  try {
    fn();
  } catch (err) {
    threw = name === undefined || (err && err.name === name);
  }
  if (!threw) throw new Error(msg);
};

class A {
  #x = 1;

  #m() {
    return this.#x + 1;
  }

  get #g() {
    return this.#x + 2;
  }

  set #g(value) {
    this.#x = value;
  }

  has(value) {
    return #x in value;
  }

  read(value) {
    return value.#x;
  }

  write(value) {
    this.#g = value;
  }

  call() {
    return this.#m();
  }

  get value() {
    return this.#g;
  }
}

class B {
  #x = 99;

  has(value) {
    return #x in value;
  }
}

const a = new A();
const b = new B();

assert(a.has(a) === true, "own private brand should be present");
assert(a.has({}) === false, "plain object should not have private brand");
assert(a.has(b) === false, "same spelling in another class should be distinct");
assert(b.has(b) === true, "other class should keep its own private brand");
assertThrows(() => a.has(1), "TypeError", "private brand check right operand should be object");
assertThrows(() => a.read({}), "TypeError", "wrong receiver private get should throw");

a.write(10);
assert(a.value === 12, "private accessors should read and write hidden state");
assert(a.call() === 11, "private methods should be callable");
assert(a["#x"] === undefined, "private field should not be a public string property");
assert(Object.keys(a).indexOf("#x") === -1, "private field should not be enumerable");
assert(Reflect.ownKeys(a).indexOf("#x") === -1, "private field should not be reflected");

class S {
  static #v = 3;

  static #inc() {
    return ++this.#v;
  }

  static has(value) {
    return #v in value;
  }

  static run() {
    return this.#inc();
  }
}

assert(S.has(S) === true, "static private brand should be on the constructor");
assert(S.has(new S()) === false, "static private brand should not be on instances");
assert(S.run() === 4, "static private methods should access static private fields");

let baseSeen = false;

class Base {
  #base = 1;

  constructor() {
    baseSeen = #base in this;
  }

  hasBase(value) {
    return #base in value;
  }
}

class Derived extends Base {
  #derived = 2;

  hasDerived(value) {
    return #derived in value;
  }
}

const d = new Derived();
assert(baseSeen === true, "base private fields should be initialized during construction");
assert(d.hasBase(d) === true, "derived instances should keep base private brands");
assert(d.hasDerived(d) === true, "derived instances should keep derived private brands");

assertThrows(() => Function("class Bad { #x; #x; }"), "SyntaxError", "duplicate private field should be syntax error");
assertThrows(() => Function("class Bad { #constructor; }"), "SyntaxError", "#constructor should be rejected");
assertThrows(() => Function("class Bad { m() { return this.#missing; } }"), "SyntaxError", "undeclared private name should be syntax error");

const makeBox = Function("return class { #x = 1; has(value) { return #x in value; } read(value) { return value.#x; } };");
const Box1 = makeBox();
const Box2 = makeBox();
const box1 = new Box1();
const box2 = new Box2();
assert(box1.has(box2) === false, "private brands from separately compiled classes should not collide");
assertThrows(() => box1.read(box2), "TypeError", "same source offsets from separate compiles should stay distinct");

class ReturnObjectBase {
  constructor(value) {
    return value;
  }
}

class OtherStamp extends ReturnObjectBase {
  #other = 1;
}

class OffsetStamp extends ReturnObjectBase {
  #x = 2;

  getX() {
    return this.#x;
  }
}

const offsetZero = new OffsetStamp({});
const offsetOne = {};
new OtherStamp(offsetOne);
new OffsetStamp(offsetOne);

assert(OffsetStamp.prototype.getX.call(offsetZero) === 2, "cached private lookup should work at offset 0");
assert(OffsetStamp.prototype.getX.call(offsetOne) === 2, "cached private lookup should validate and recover at offset 1");
assert(OffsetStamp.prototype.getX.call(offsetZero) === 2, "cached private lookup should validate when returning to offset 0");

console.log("private brand tests ok");
