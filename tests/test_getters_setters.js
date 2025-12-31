class Person {
  constructor(name) {
    this._name = name;
  }

  get name() {
    console.log("Getting name");
    return this._name;
  }

  set name(value) {
    console.log("Setting name to:", value);
    this._name = value;
  }

  get greeting() {
    return "Hello, " + this._name;
  }
}

const person = new Person("Alice");
console.log("1. Initial name:", person.name);
console.log("2. Greeting:", person.greeting);

person.name = "Bob";
console.log("3. After setting name:", person.name);
console.log("4. New greeting:", person.greeting);

// Test with multiple getters
class Rectangle {
  constructor(width, height) {
    this._width = width;
    this._height = height;
  }

  get area() {
    return this._width * this._height;
  }

  get perimeter() {
    return 2 * (this._width + this._height);
  }

  set width(w) {
    console.log("Setting width to:", w);
    this._width = w;
  }

  get width() {
    return this._width;
  }
}

const rect = new Rectangle(5, 10);
console.log("5. Rectangle area:", rect.area);
console.log("6. Rectangle perimeter:", rect.perimeter);
