// Test binding native event functions

console.log('Testing .bind() with native functions...\n');

// Test 1: Bind addEventListener
console.log('Test 1: Bind addEventListener');
const target = new EventTarget();
const boundAdd = target.addEventListener.bind(target);

console.log('  boundAdd type:', typeof boundAdd);

try {
  boundAdd('test', () => {
    console.log('  Listener called!');
  });
  target.dispatchEvent('test');
  console.log('  PASS: Bound native function works!');
} catch (e) {
  console.log('  FAIL:', e);
}

// Test 2: Store bound method in object
console.log('\nTest 2: Store bound methods in object');
const target2 = new EventTarget();
const wrapper = {};
wrapper.on = target2.addEventListener.bind(target2);
wrapper.emit = target2.dispatchEvent.bind(target2);

try {
  wrapper.on('test', () => {
    console.log('  Stored bound listener called!');
  });
  wrapper.emit('test');
  console.log('  PASS: Stored bound methods work!');
} catch (e) {
  console.log('  FAIL:', e);
}

// Test 3: Use in a class
console.log('\nTest 3: Use bound methods in class');
class MyEmitter {
  constructor() {
    const target = new EventTarget();
    this.on = target.addEventListener.bind(target);
    this.emit = target.dispatchEvent.bind(target);
  }
}

try {
  const emitter = new MyEmitter();
  emitter.on('test', () => {
    console.log('  Class listener called!');
  });
  emitter.emit('test');
  console.log('  PASS: Class with bound methods works!');
} catch (e) {
  console.log('  FAIL:', e);
}

console.log('\nAll tests completed!');
