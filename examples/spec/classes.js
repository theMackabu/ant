import { test, summary } from './helpers.js';

console.log('Class Tests\n');

class Person {
  constructor(name, age) {
    this.name = name;
    this.age = age;
  }
}

let person = new Person('Alice', 30);
test('constructor sets name', person.name, 'Alice');
test('constructor sets age', person.age, 30);

class Calculator {
  constructor(value) {
    this.value = value;
  }
  add(n) {
    this.value = this.value + n;
    return this.value;
  }
  subtract(n) {
    this.value = this.value - n;
    return this.value;
  }
}

let calc = new Calculator(10);
test('method add', calc.add(5), 15);
test('method subtract', calc.subtract(3), 12);

class Point {
  setCoords(x, y) {
    this.x = x;
    this.y = y;
  }
  getDistance() {
    return this.x * this.x + this.y * this.y;
  }
}

let p = new Point();
p.setCoords(3, 4);
test('no constructor class', p.getDistance(), 25);

class Counter {
  constructor(start) {
    this.count = start;
  }
  increment() {
    this.count = this.count + 1;
    return this.count;
  }
}

let c1 = new Counter(0);
let c2 = new Counter(10);
test('instance isolation c1', c1.increment(), 1);
test('instance isolation c2', c2.increment(), 11);

test('typeof instance', typeof person, 'object');
test('instanceof Object', person instanceof Object, true);

class Rectangle {
  constructor(width, height) {
    this.width = width;
    this.height = height;
  }
  area() {
    return this.width * this.height;
  }
}

let rect = new Rectangle(5, 10);
test('method uses props', rect.area(), 50);
rect.width = 8;
test('modified prop', rect.area(), 80);

let p2 = new Person('Charlie', 35);
delete p2.name;
test('delete property', p2.name, undefined);
test('other prop still exists', p2.age, 35);

class Chainable {
  constructor(value) {
    this.value = value;
  }
  add(n) {
    this.value = this.value + n;
    return this;
  }
  multiply(n) {
    this.value = this.value * n;
    return this;
  }
  get() {
    return this.value;
  }
}

let chain = new Chainable(5);
test('method chaining', chain.add(3).multiply(2).get(), 16);

class Feature {
  constructor(name) {
    this.name = name;
    this.enabled = false;
  }
  enable() {
    this.enabled = true;
  }
  disable() {
    this.enabled = false;
  }
}

let feature = new Feature('DarkMode');
test('initial bool prop', feature.enabled, false);
feature.enable();
test('toggle bool prop', feature.enabled, true);

class Animal {
  constructor(name) {
    this.name = name;
  }
  speak() {
    return this.name + ' makes a sound';
  }
}

class Dog extends Animal {
  speak() {
    return this.name + ' barks';
  }
}

let dog = new Dog('Rex');
test('inheritance', dog.speak(), 'Rex barks');
test('instanceof parent', dog instanceof Animal, true);
test('instanceof child', dog instanceof Dog, true);

class Static {
  static value = 42;
  static method() {
    return 'static';
  }
}

test('static property', Static.value, 42);
test('static method', Static.method(), 'static');

summary();
