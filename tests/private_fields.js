// Test private fields in classes

// Basic private field access
class Rectangle {
  #width;
  #height;
  
  constructor(w, h) {
    this.#width = w;
    this.#height = h;
  }
  
  area() {
    return this.#width * this.#height;
  }
}

const rect = new Rectangle(5, 10);
console.assert(rect.area() === 50, "Rectangle area should be 50");

// Private field with getter/setter
class Person {
  #name;
  #age;
  
  constructor(name, age) {
    this.#name = name;
    this.#age = age;
  }
  
  getName() {
    return this.#name;
  }
  
  getAge() {
    return this.#age;
  }
  
  setAge(newAge) {
    this.#age = newAge;
  }
  
  describe() {
    return this.#name + " is " + this.#age;
  }
}

const person = new Person("Alice", 30);
console.assert(person.getName() === "Alice", "getName should return Alice");
console.assert(person.getAge() === 30, "getAge should return 30");
console.assert(person.describe() === "Alice is 30", "describe should work");
person.setAge(31);
console.assert(person.describe() === "Alice is 31", "setAge should work");

// Private fields with inheritance
class Animal {
  #species;
  
  constructor(species) {
    this.#species = species;
  }
  
  getSpecies() {
    return this.#species;
  }
}

class Dog extends Animal {
  #name;
  
  constructor(name, species) {
    super(species);
    this.#name = name;
  }
  
  getName() {
    return this.#name;
  }
}

const dog = new Dog("Rex", "Canis familiaris");
console.assert(dog.getName() === "Rex", "Dog getName should return Rex");
console.assert(dog.getSpecies() === "Canis familiaris", "Dog getSpecies should work");

// Private field with initializer
class Counter {
  #count = 0;
  
  constructor(start) {
    this.#count = start;
  }
  
  increment() {
    this.#count = this.#count + 1;
    return this.#count;
  }
  
  getCount() {
    return this.#count;
  }
}

const counter = new Counter(10);
console.assert(counter.increment() === 11, "Counter increment should return 11");
console.assert(counter.increment() === 12, "Counter increment should return 12");
console.assert(counter.getCount() === 12, "Counter getCount should return 12");

// Performance test: tight loop with private fields and initializer
class FastCounter {
  #n = 0;
  
  increment() {
    this.#n = this.#n + 1;
  }
  
  get() {
    return this.#n;
  }
}

const fast = new FastCounter();
for (let i = 0; i < 1000; i = i + 1) {
  fast.increment();
}
console.assert(fast.get() === 1000, "FastCounter should reach 1000");

console.log("All private fields tests passed!");
