import stuff from './stuff.js';
import { add, mul, PI } from './math';

const value = mul(2, 3);
const result = add(value, 3);

queueMicrotask(() => {
  console.log('hello world');
  console.log(`current dir is '${__dirname}'`);
});

console.log(this, '\n');

function main() {
  console.log(result);
  console.log(PI);
  console.log(stuff.test());

  console.log(typeof result);

  console.log(value instanceof Number);
  console.log(String(123));
}

void main();
