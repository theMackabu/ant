const assert = require('node:assert');

function T() { throw 1; }

function finallyContinue(n) {
  let s = 0;
  for (let j = 0; j < 2; j++) {
    try {
      for (let i = 0; i < n; i++) s += i;
      if (j === 0) continue;
      break;
    } finally {
      try { T(); } catch (e) { s++; }
    }
    s += 1000000;
  }
  return s;
}

function finallyReturn(n) {
  let s = 0;
  try {
    for (let i = 0; i < n; i++) s += i;
    return 'ret' + s;
  } finally {
    try { T(); } catch (e) { s++; }
  }
  return 'end';
}

function finallyThrow(n) {
  let s = 0;
  try {
    try {
      for (let i = 0; i < n; i++) s += i;
      throw 'orig';
    } finally {
      try { T(); } catch (e) { s++; }
    }
  } catch (e) {
    return e + s;
  }
  return 'end';
}

function finallyShortLoop(n) {
  let s = 0;
  try {
    for (let i = 0; i < n; i++) s += i;
    return 'ret' + s;
  } finally {
    try { T(); } catch (e) { s++; }
  }
  return 'end';
}

for (let i = 0; i < 8; i++) {
  assert.strictEqual(finallyContinue(3000), 8997002);
  assert.strictEqual(finallyReturn(3000), 'ret4498500');
  assert.strictEqual(finallyThrow(3000), 'orig4498501');
}
for (let i = 0; i < 120; i++) assert.strictEqual(finallyShortLoop(10), 'ret45', 'call ' + i);

console.log('ok');
