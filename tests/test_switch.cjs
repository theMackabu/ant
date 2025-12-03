// Test basic switch statement
let x = 2;
let result = 0;

switch (x) {
  case 1:
    result = 10;
    break;
  case 2:
    result = 20;
    break;
  case 3:
    result = 30;
    break;
  default:
    result = 99;
}

console.log('Result:', result); // Should be 20

// Test switch without break (fall-through)
let y = 1;
let sum = 0;

switch (y) {
  case 1:
    sum = sum + 1;
  case 2:
    sum = sum + 2;
  case 3:
    sum = sum + 3;
    break;
  default:
    sum = sum + 100;
}

console.log('Sum:', sum); // Should be 6 (1+2+3)

// Test switch with string
let fruit = 'apple';
let color = '';

switch (fruit) {
  case 'apple':
    color = 'red';
    break;
  case 'banana':
    color = 'yellow';
    break;
  case 'grape':
    color = 'purple';
    break;
  default:
    color = 'unknown';
}

console.log('Color:', color); // Should be "red"

// Test switch with default only
let z = 5;
let msg = '';

switch (z) {
  case 1:
    msg = 'one';
    break;
  case 2:
    msg = 'two';
    break;
  default:
    msg = 'other';
}

console.log('Message:', msg); // Should be "other"
