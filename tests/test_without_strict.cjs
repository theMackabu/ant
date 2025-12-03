// Test 1: with statement should work without 'use strict'
let obj = { x: 10, y: 20 };
with (obj) {
  console.log("Test 1 - x:", x); // should print 10
  console.log("Test 1 - y:", y); // should print 20
}
console.log("Test 1 passed");
