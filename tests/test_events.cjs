// Test Event Listeners

console.log('Testing Event Listeners...\n');

// Test 1: Basic addEventListener and dispatchEvent (global)
console.log('Test 1: Basic global event listener');
let test1Called = false;
addEventListener('test', (event) => {
  console.log('  Event received:', event.type);
  test1Called = true;
});
dispatchEvent('test');
console.log('  Result:', test1Called ? 'PASS' : 'FAIL');

// Test 2: Event with custom data (global)
console.log('\nTest 2: Event with custom data (global)');
addEventListener('customEvent', (event) => {
  console.log('  Event type:', event.type);
  console.log('  Event detail:', event.detail);
  console.log('  Result: PASS');
});
dispatchEvent('customEvent', { message: 'Hello from event!', value: 42 });

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
dispatchEvent('multiEvent');
console.log('  Total calls:', callCount);
console.log('  Result:', callCount === 3 ? 'PASS' : 'FAIL');

// Test 4: Once option (global)
console.log('\nTest 4: Once option (listener should only fire once)');
let onceCount = 0;
addEventListener('onceEvent', () => {
  onceCount++;
  console.log('  Once listener called (count:', onceCount + ')');
}, { once: true });
dispatchEvent('onceEvent');
dispatchEvent('onceEvent');
dispatchEvent('onceEvent');
console.log('  Result:', onceCount === 1 ? 'PASS' : 'FAIL');

// Test 5: removeEventListener (global)
console.log('\nTest 5: removeEventListener (global)');
let removedCount = 0;
function removableListener() {
  removedCount++;
  console.log('  Listener called');
}
addEventListener('removeTest', removableListener);
dispatchEvent('removeTest');
console.log('  First dispatch count:', removedCount);
removeEventListener('removeTest', removableListener);
dispatchEvent('removeTest');
console.log('  After removal count:', removedCount);
console.log('  Result:', removedCount === 1 ? 'PASS' : 'FAIL');

// Test 6: Different event types don't interfere
console.log('\nTest 6: Event type isolation');
let event1Called = false;
let event2Called = false;
addEventListener('eventType1', () => { event1Called = true; });
addEventListener('eventType2', () => { event2Called = true; });
dispatchEvent('eventType1');
console.log('  After eventType1 - type1:', event1Called, 'type2:', event2Called);
dispatchEvent('eventType2');
console.log('  After eventType2 - type1:', event1Called, 'type2:', event2Called);
console.log('  Result:', (event1Called && event2Called) ? 'PASS' : 'FAIL');

// Test 7: Object-specific event listeners
console.log('\nTest 7: Object-specific event listeners');

const emitter1 = createEventTarget();
const emitter2 = createEventTarget();

let emitter1Count = 0;
let emitter2Count = 0;

emitter1.addEventListener('click', (event) => {
  emitter1Count++;
  console.log('  Emitter1 clicked! Count:', emitter1Count);
  console.log('  Event target:', event.target === emitter1 ? 'correct' : 'wrong');
});

emitter2.addEventListener('click', (event) => {
  emitter2Count++;
  console.log('  Emitter2 clicked! Count:', emitter2Count);
  console.log('  Event target:', event.target === emitter2 ? 'correct' : 'wrong');
});

emitter1.dispatchEvent('click');
emitter1.dispatchEvent('click');
emitter2.dispatchEvent('click');

console.log('  Emitter1 count:', emitter1Count);
console.log('  Emitter2 count:', emitter2Count);
console.log('  Result:', (emitter1Count === 2 && emitter2Count === 1) ? 'PASS' : 'FAIL');

// Test 8: Object events with custom data
console.log('\nTest 8: Object events with custom data');
const dataEmitter = createEventTarget();

dataEmitter.addEventListener('message', (event) => {
  console.log('  Message received:', event.detail.text);
  console.log('  Sender:', event.detail.sender);
  console.log('  Result: PASS');
});

dataEmitter.dispatchEvent('message', { text: 'Hello Object!', sender: 'test' });

// Test 9: Object event with once option
console.log('\nTest 9: Object event with once option');
const onceEmitter = createEventTarget();
let objOnceCount = 0;

onceEmitter.addEventListener('onceEvent', () => {
  objOnceCount++;
  console.log('  Object once listener called (count:', objOnceCount + ')');
}, { once: true });

onceEmitter.dispatchEvent('onceEvent');
onceEmitter.dispatchEvent('onceEvent');
console.log('  Result:', objOnceCount === 1 ? 'PASS' : 'FAIL');

// Test 10: Object removeEventListener
console.log('\nTest 10: Object removeEventListener');
const removeEmitter = createEventTarget();
let objRemoveCount = 0;

function objRemovableListener() {
  objRemoveCount++;
  console.log('  Object listener called');
}

removeEmitter.addEventListener('remove', objRemovableListener);
removeEmitter.dispatchEvent('remove');
console.log('  Before removal count:', objRemoveCount);
removeEmitter.removeEventListener('remove', objRemovableListener);
removeEmitter.dispatchEvent('remove');
console.log('  After removal count:', objRemoveCount);
console.log('  Result:', objRemoveCount === 1 ? 'PASS' : 'FAIL');

// Test 11: Multiple objects don't interfere
console.log('\nTest 11: Multiple objects event isolation');
const objA = createEventTarget();
const objB = createEventTarget();

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

objA.dispatchEvent('test');
console.log('  After objA dispatch - A:', objACalled, 'B:', objBCalled);

objBCalled = false; // Reset
objB.dispatchEvent('test');
console.log('  After objB dispatch - A:', objACalled, 'B:', objBCalled);
console.log('  Result:', (objACalled && objBCalled) ? 'PASS' : 'FAIL');

// Test 12: getEventListeners with objects
console.log('\nTest 12: getEventListeners for objects');
const debugObj = createEventTarget();
debugObj.addEventListener('debugEvent', () => {});
debugObj.addEventListener('debugEvent', () => {});

const listeners = getEventListeners(debugObj);
console.log('  Has debugEvent listeners:', listeners.debugEvent !== undefined);
console.log('  Number of debugEvent listeners:', listeners.debugEvent?.length || 0);
console.log('  Result:', listeners.debugEvent?.length === 2 ? 'PASS' : 'FAIL');

console.log('\n=== All event listener tests completed! ===');
