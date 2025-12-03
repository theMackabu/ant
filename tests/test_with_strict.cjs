"use strict";

// Test 2: with statement should fail with 'use strict'
let obj = { x: 10, y: 20 };
with (obj) {
  console.log("This should not run");
}
