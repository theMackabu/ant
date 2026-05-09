// Test Event Listeners

const events = require('node:events');

console.log('Testing Event Listeners...\n');

// Test 1: Basic addEventListener and dispatchEvent (global)
console.log('Test 1: Basic global event listener');
let test1Called = false;
addEventListener('test', event => {
  console.log('  Event received:', event.type);
  test1Called = true;
});
dispatchEvent(new Event('test'));
console.log('  Result:', test1Called ? 'PASS' : 'FAIL');

// Test 2: Event with custom data (global)
console.log('\nTest 2: Event with custom data (global)');
addEventListener('customEvent', event => {
  console.log('  Event type:', event.type);
  console.log('  Event detail:', event.detail);
  console.log('  Result: PASS');
});
dispatchEvent(
  new CustomEvent('customEvent', {
    detail: { message: 'Hello from event!', value: 42 }
  })
);

// Test 3: Multiple listeners for same event (global)
console.log('\nTest 3: Multiple listeners for same event (global)');
let callCount = 0;
addEventListener('multiEvent', () => {
  callCount++;
  console.log('  Listener 1 called');
});
addEventListener('multiEvent', () => {
  callCount++;
  console.log('  Listener 2 called');
});
addEventListener('multiEvent', () => {
  callCount++;
  console.log('  Listener 3 called');
});
dispatchEvent(new Event('multiEvent'));
console.log('  Total calls:', callCount);
console.log('  Result:', callCount === 3 ? 'PASS' : 'FAIL');

// Test 4: Once option (global)
console.log('\nTest 4: Once option (listener should only fire once)');
let onceCount = 0;
addEventListener(
  'onceEvent',
  () => {
    onceCount++;
    console.log('  Once listener called (count:', onceCount + ')');
  },
  { once: true }
);
dispatchEvent(new Event('onceEvent'));
dispatchEvent(new Event('onceEvent'));
dispatchEvent(new Event('onceEvent'));
console.log('  Result:', onceCount === 1 ? 'PASS' : 'FAIL');

// Test 5: removeEventListener (global)
console.log('\nTest 5: removeEventListener (global)');
let removedCount = 0;
function removableListener() {
  removedCount++;
  console.log('  Listener called');
}
addEventListener('removeTest', removableListener);
dispatchEvent(new Event('removeTest'));
console.log('  First dispatch count:', removedCount);
removeEventListener('removeTest', removableListener);
dispatchEvent(new Event('removeTest'));
console.log('  After removal count:', removedCount);
console.log('  Result:', removedCount === 1 ? 'PASS' : 'FAIL');

// Test 6: Different event types don't interfere
console.log('\nTest 6: Event type isolation');
let event1Called = false;
let event2Called = false;
addEventListener('eventType1', () => {
  event1Called = true;
});
addEventListener('eventType2', () => {
  event2Called = true;
});
dispatchEvent(new Event('eventType1'));
console.log('  After eventType1 - type1:', event1Called, 'type2:', event2Called);
dispatchEvent(new Event('eventType2'));
console.log('  After eventType2 - type1:', event1Called, 'type2:', event2Called);
console.log('  Result:', event1Called && event2Called ? 'PASS' : 'FAIL');

// Test 7: Object-specific event listeners
console.log('\nTest 7: Object-specific event listeners');

const emitter1 = new EventTarget();
const emitter2 = new EventTarget();

let emitter1Count = 0;
let emitter2Count = 0;

emitter1.addEventListener('click', event => {
  emitter1Count++;
  console.log('  Emitter1 clicked! Count:', emitter1Count);
  console.log('  Event target:', event.target === emitter1 ? 'correct' : 'wrong');
});

emitter2.addEventListener('click', event => {
  emitter2Count++;
  console.log('  Emitter2 clicked! Count:', emitter2Count);
  console.log('  Event target:', event.target === emitter2 ? 'correct' : 'wrong');
});

emitter1.dispatchEvent(new Event('click'));
emitter1.dispatchEvent(new Event('click'));
emitter2.dispatchEvent(new Event('click'));

console.log('  Emitter1 count:', emitter1Count);
console.log('  Emitter2 count:', emitter2Count);
console.log('  Result:', emitter1Count === 2 && emitter2Count === 1 ? 'PASS' : 'FAIL');

// Test 8: Object events with custom data
console.log('\nTest 8: Object events with custom data');
const dataEmitter = new EventTarget();

dataEmitter.addEventListener('message', event => {
  console.log('  Message received:', event.detail.text);
  console.log('  Sender:', event.detail.sender);
  console.log('  Result: PASS');
});

dataEmitter.dispatchEvent(
  new CustomEvent('message', {
    detail: { text: 'Hello Object!', sender: 'test' }
  })
);

// Test 9: Object event with once option
console.log('\nTest 9: Object event with once option');
const onceEmitter = new EventTarget();
let objOnceCount = 0;

onceEmitter.addEventListener(
  'onceEvent',
  () => {
    objOnceCount++;
    console.log('  Object once listener called (count:', objOnceCount + ')');
  },
  { once: true }
);

onceEmitter.dispatchEvent(new Event('onceEvent'));
onceEmitter.dispatchEvent(new Event('onceEvent'));
console.log('  Result:', objOnceCount === 1 ? 'PASS' : 'FAIL');

// Test 10: Object removeEventListener
console.log('\nTest 10: Object removeEventListener');
const removeEmitter = new EventTarget();
let objRemoveCount = 0;

function objRemovableListener() {
  objRemoveCount++;
  console.log('  Object listener called');
}

removeEmitter.addEventListener('remove', objRemovableListener);
removeEmitter.dispatchEvent(new Event('remove'));
console.log('  Before removal count:', objRemoveCount);
removeEmitter.removeEventListener('remove', objRemovableListener);
removeEmitter.dispatchEvent(new Event('remove'));
console.log('  After removal count:', objRemoveCount);
console.log('  Result:', objRemoveCount === 1 ? 'PASS' : 'FAIL');

// Test 11: Multiple objects don't interfere
console.log('\nTest 11: Multiple objects event isolation');
const objA = new EventTarget();
const objB = new EventTarget();

let objACalled = false;
let objBCalled = false;

objA.addEventListener('test', () => {
  objACalled = true;
  console.log('  ObjA test fired');
});
objB.addEventListener('test', () => {
  objBCalled = true;
  console.log('  ObjB test fired');
});

objA.dispatchEvent(new Event('test'));
console.log('  After objA dispatch - A:', objACalled, 'B:', objBCalled);

objBCalled = false; // Reset
objB.dispatchEvent(new Event('test'));
console.log('  After objB dispatch - A:', objACalled, 'B:', objBCalled);
console.log('  Result:', objACalled && objBCalled ? 'PASS' : 'FAIL');

// Test 12: events.getEventListeners with EventTarget
console.log('\nTest 12: events.getEventListeners for EventTarget');
const debugTarget = new EventTarget();
debugTarget.addEventListener('debugEvent', () => {});
debugTarget.addEventListener('debugEvent', () => {});

const listeners = events.getEventListeners(debugTarget, 'debugEvent');
console.log('  Number of debugEvent listeners:', listeners.length);
console.log('  Result:', listeners.length === 2 ? 'PASS' : 'FAIL');

console.log('\n=== All event listener tests completed! ===');
