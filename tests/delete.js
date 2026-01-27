const user = {
  name: 'Alice',
  age: 30,
  active: true
};

Ant.gc();
console.log('GC requested');

console.log(user.age); // Output: 30

delete user.age; // The 'delete' operator returns true on success

console.log(user.age); // Output: undefined
console.log(user); // Output: { name: 'Alice', active: true }
Ant.gc();
console.log('GC requested');
