function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// DOM dispatch differs from node's emit(): the spec clones the listener list but
// re-checks each listener's "removed" flag at invocation time, so a listener
// unregistered by an earlier listener in the same dispatch must not run.
// EventEmitter.emit() deliberately does the opposite.
const target = new EventTarget();
const fired = [];
const removed = () => fired.push('removed');

target.addEventListener('go', () => {
  fired.push('first');
  target.removeEventListener('go', removed);
});
target.addEventListener('go', removed);
target.dispatchEvent(new Event('go'));

assert(
  fired.join(',') === 'first',
  `a listener removed mid-dispatch must not fire, got ${fired.join(',')}`
);

// a listener added mid-dispatch must not run in that dispatch either.
// NOTE: node deviates from the DOM spec here and does run it, so this block
// intentionally does not match `node tests/test_eventtarget_dispatch_removal.cjs`
const added = [];
const addTarget = new EventTarget();
addTarget.addEventListener('go', () => {
  added.push('first');
  addTarget.addEventListener('go', () => added.push('late'));
});

addTarget.dispatchEvent(new Event('go'));
assert(added.join(',') === 'first', `listener added mid-dispatch should wait, got ${added.join(',')}`);

addTarget.dispatchEvent(new Event('go'));
assert(
  added.join(',') === 'first,first,late',
  `listener added mid-dispatch should run on the next dispatch, got ${added.join(',')}`
);

// once listeners still fire exactly once through the DOM path
const onceFired = [];
const onceTarget = new EventTarget();
onceTarget.addEventListener('go', () => onceFired.push('once'), { once: true });
onceTarget.dispatchEvent(new Event('go'));
onceTarget.dispatchEvent(new Event('go'));
assert(onceFired.join(',') === 'once', `once listener should fire once, got ${onceFired.join(',')}`);

console.log('PASS');
