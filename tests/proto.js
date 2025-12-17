function Person(first, last) {
  this.firstName = first;
  this.lastName = last;
}

Person.prototype.fullName = function () {
  return this.firstName + ' ' + this.lastName;
};

const person1 = new Person('John', 'Doe');
const person2 = new Person('Jane', 'Smith');

console.log(person1.fullName());
console.log(person2.fullName());

console.log(Person.prototype);
