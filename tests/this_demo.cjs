// Demonstration of 'this' keyword and 'new' operator

Ant.println("=== Constructor Functions with 'new' and 'this' ===\n");

// Basic constructor function
function Person(name, age) {
  this.name = name;
  this.age = age;
  this.greet = function() {
    return "Hello, my name is " + this.name + " and I am " + this.age + " years old.";
  };
}

// Create instances using 'new'
Ant.println("Creating person1 with new Person('Alice', 30)");
let person1 = new Person("Alice", 30);
Ant.println("person1.name:", person1.name);
Ant.println("person1.age:", person1.age);
Ant.println("person1.greet():", person1.greet());
Ant.println();

Ant.println("Creating person2 with new Person('Bob', 25)");
let person2 = new Person("Bob", 25);
Ant.println("person2.name:", person2.name);
Ant.println("person2.age:", person2.age);
Ant.println("person2.greet():", person2.greet());
Ant.println();

// Each instance has its own 'this' context
Ant.println("=== Independent 'this' contexts ===");
Ant.println("person1.name:", person1.name);
Ant.println("person2.name:", person2.name);
Ant.println("They are different instances with independent 'this' values");
Ant.println();

// Constructor with methods that use 'this'
function Counter(start) {
  this.value = start;
  this.increment = function() {
    this.value = this.value + 1;
    return this.value;
  };
  this.decrement = function() {
    this.value = this.value - 1;
    return this.value;
  };
  this.getValue = function() {
    return this.value;
  };
}

Ant.println("=== Counter Example ===");
let counter = new Counter(10);
Ant.println("Initial value:", counter.getValue());
Ant.println("After increment:", counter.increment());
Ant.println("After increment:", counter.increment());
Ant.println("After decrement:", counter.decrement());
Ant.println("Final value:", counter.getValue());
Ant.println();

// Multiple counters with independent state
Ant.println("=== Multiple Independent Counters ===");
let counter1 = new Counter(0);
let counter2 = new Counter(100);

Ant.println("counter1 initial:", counter1.getValue());
Ant.println("counter2 initial:", counter2.getValue());

counter1.increment();
counter1.increment();
counter2.decrement();

Ant.println("counter1 after 2 increments:", counter1.getValue());
Ant.println("counter2 after 1 decrement:", counter2.getValue());
Ant.println();

// Constructor with computed properties
function Rectangle(width, height) {
  this.width = width;
  this.height = height;
  this.area = function() {
    return this.width * this.height;
  };
  this.perimeter = function() {
    return 2 * (this.width + this.height);
  };
}

Ant.println("=== Rectangle Example ===");
let rect1 = new Rectangle(5, 10);
Ant.println("Rectangle 5x10:");
Ant.println("  Width:", rect1.width);
Ant.println("  Height:", rect1.height);
Ant.println("  Area:", rect1.area());
Ant.println("  Perimeter:", rect1.perimeter());
Ant.println();

let rect2 = new Rectangle(3, 7);
Ant.println("Rectangle 3x7:");
Ant.println("  Width:", rect2.width);
Ant.println("  Height:", rect2.height);
Ant.println("  Area:", rect2.area());
Ant.println("  Perimeter:", rect2.perimeter());
Ant.println();

// 'this' refers to the object being constructed
function Car(make, model, year) {
  this.make = make;
  this.model = model;
  this.year = year;
  this.displayInfo = function() {
    return this.year + " " + this.make + " " + this.model;
  };
}

Ant.println("=== Car Example ===");
let car1 = new Car("Toyota", "Camry", 2020);
let car2 = new Car("Honda", "Civic", 2021);

Ant.println("car1:", car1.displayInfo());
Ant.println("car2:", car2.displayInfo());
Ant.println();

// 'this' in nested functions
function Account(owner, balance) {
  this.owner = owner;
  this.balance = balance;
  
  this.deposit = function(amount) {
    this.balance = this.balance + amount;
    return this.balance;
  };
  
  this.withdraw = function(amount) {
    this.balance = this.balance - amount;
    return this.balance;
  };
  
  this.getInfo = function() {
    return this.owner + " has $" + this.balance;
  };
}

Ant.println("=== Bank Account Example ===");
let account1 = new Account("Alice", 1000);
Ant.println("Initial:", account1.getInfo());

account1.deposit(500);
Ant.println("After deposit 500:", account1.getInfo());

account1.withdraw(200);
Ant.println("After withdraw 200:", account1.getInfo());
Ant.println();

// Multiple accounts
let account2 = new Account("Bob", 2000);
Ant.println("account1:", account1.getInfo());
Ant.println("account2:", account2.getInfo());
Ant.println();

Ant.println("=== Summary ===");
Ant.println("- 'new' creates a new object");
Ant.println("- 'this' inside constructor refers to the new object");
Ant.println("- Each instance has its own 'this' context");
Ant.println("- Methods can access and modify 'this' properties");
