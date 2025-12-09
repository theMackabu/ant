// Debug native function binding

const target = createEventTarget();

console.log('target:', target);
console.log('typeof target:', typeof target);

const addEventListenerFn = target.addEventListener;
console.log('\naddEventListenerFn:', addEventListenerFn);
console.log('typeof addEventListenerFn:', typeof addEventListenerFn);

console.log('\nTrying to call it directly:');
try {
  addEventListenerFn.call(target, 'test', () => {
    console.log('Direct call worked!');
  });
  target.dispatchEvent('test');
} catch (e) {
  console.log('Error:', e);
}

console.log('\nTrying to bind it:');
try {
  const bound = addEventListenerFn.bind(target);
  console.log('bound:', bound);
  console.log('typeof bound:', typeof bound);
  
  bound('test', () => {
    console.log('Bound call worked!');
  });
  target.dispatchEvent('test');
} catch (e) {
  console.log('Error:', e);
}
