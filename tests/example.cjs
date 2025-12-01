const math = Ant.require('./math.cjs');
const stuff = Ant.require('./stuff.cjs');

const value = math.mul(2, 3);
const result = math.add(value, 3);

console.log(this);
console.log();

function main() {
  console.log(result);
  console.log(math.PI);
  console.log(stuff.test());

  console.log(Ant.__dirname);
  console.log(typeof result);

  console.log(value instanceof Number);
  console.log(String(123));
}

void main();
