const math = Ant.require('./math.cjs');
const stuff = Ant.require('./stuff.cjs');

const value = math.mul(2, 3);
const result = math.add(value, 3);

Ant.println(this);
Ant.println();

function main() {
  Ant.println(result);
  Ant.println(math.PI);
  Ant.println(stuff.test());

  Ant.println(Ant.__dirname);
  Ant.println(typeof result);

  Ant.println(value instanceof Number);
  Ant.println(String(123));
}

void main();
