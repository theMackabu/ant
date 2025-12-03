const person = {
  name: 'Alice',
  age: 30,
  city: 'New York'
};

for (let key in person) {
  console.log(`${key}: ${person[key]}`);
}
