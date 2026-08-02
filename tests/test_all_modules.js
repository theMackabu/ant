// Test toStringTag for all modules

console.log('Testing Symbol.toStringTag for all modules:\n');

// Built-in modules
console.log('Atomics:', Object.prototype.toString.call(Atomics));
console.log('console:', Object.prototype.toString.call(console));
console.log('JSON:', Object.prototype.toString.call(JSON));
console.log('process:', Object.prototype.toString.call(process));
console.log('Buffer:', Object.prototype.toString.call(Buffer));

// Test crypto if available
if (typeof Ant !== 'undefined' && crypto) {
  console.log('crypto:', Object.prototype.toString.call(crypto));
}

// Test imported modules
console.log('\nTesting imported modules:');

// Import path module
import * as path from 'ant:path';
console.log('path:', Object.prototype.toString.call(path));

// Import fs module
import * as fs from 'ant:fs';
console.log('fs:', Object.prototype.toString.call(fs));

// Import shell module
import * as shell from 'ant:shell';
console.log('shell:', Object.prototype.toString.call(shell));

// Import ffi module
import * as ffi from 'ant:ffi';
console.log('ffi:', Object.prototype.toString.call(ffi));
