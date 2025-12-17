import { test, summary } from './helpers.js';

console.log('Switch Tests\n');

let x = 2;
let result = 0;
switch (x) {
  case 1: result = 10; break;
  case 2: result = 20; break;
  case 3: result = 30; break;
  default: result = 99;
}
test('switch case 2', result, 20);

let y = 1;
let sum = 0;
switch (y) {
  case 1: sum += 1;
  case 2: sum += 2;
  case 3: sum += 3; break;
  default: sum += 100;
}
test('switch fall-through', sum, 6);

let fruit = 'apple';
let color = '';
switch (fruit) {
  case 'apple': color = 'red'; break;
  case 'banana': color = 'yellow'; break;
  case 'grape': color = 'purple'; break;
  default: color = 'unknown';
}
test('switch string', color, 'red');

let z = 5;
let msg = '';
switch (z) {
  case 1: msg = 'one'; break;
  case 2: msg = 'two'; break;
  default: msg = 'other';
}
test('switch default', msg, 'other');

let a = 0;
let aResult = '';
switch (a) {
  case 0: aResult = 'zero'; break;
  case 1: aResult = 'one'; break;
}
test('switch case 0', aResult, 'zero');

let b = true;
let bResult = '';
switch (b) {
  case true: bResult = 'true'; break;
  case false: bResult = 'false'; break;
}
test('switch boolean', bResult, 'true');

function getGrade(score) {
  switch (true) {
    case score >= 90: return 'A';
    case score >= 80: return 'B';
    case score >= 70: return 'C';
    default: return 'F';
  }
}
test('switch expression A', getGrade(95), 'A');
test('switch expression B', getGrade(85), 'B');
test('switch expression F', getGrade(50), 'F');

summary();
