const { EventEmitter } = require('events');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// node clones the listener array before dispatching, so a listener added during
// an emit only runs on the next one
const added = [];
const addEmitter = new EventEmitter();
const lateListener = () => added.push('late');

addEmitter.on('go', () => {
  added.push('first');
  addEmitter.on('go', lateListener);
});

addEmitter.emit('go');
assert(
  added.join(',') === 'first',
  `listener added during emit should wait for the next emit, got ${added.join(',')}`
);

addEmitter.emit('go');
assert(
  added.join(',') === 'first,first,late',
  `listener added during emit should run on the next emit, got ${added.join(',')}`
);

// a listener removed mid-dispatch still runs, and removing it must not cause the
// dispatch loop to skip the listener that shifts into its place
const removal = [];
const removalEmitter = new EventEmitter();
const removesItself = () => {
  removal.push('self');
  removalEmitter.removeListener('go', removesItself);
};
const shouldStillRun = () => removal.push('sibling');

removalEmitter.on('go', removesItself);
removalEmitter.on('go', shouldStillRun);
removalEmitter.emit('go');
assert(
  removal.join(',') === 'self,sibling',
  `removing a listener mid-emit must not skip the next listener, got ${removal.join(',')}`
);

removal.length = 0;
removalEmitter.emit('go');
assert(
  removal.join(',') === 'sibling',
  `removed listener should be gone on the next emit, got ${removal.join(',')}`
);

// a once listener must not be invoked twice when a listener re-emits synchronously
const nested = [];
const nestedEmitter = new EventEmitter();
let reentered = false;

nestedEmitter.on('go', () => {
  nested.push('on');
  if (reentered) return;
  reentered = true;
  nestedEmitter.emit('go');
});
nestedEmitter.once('go', () => nested.push('once'));
nestedEmitter.emit('go');
assert(
  nested.join(',') === 'on,on,once',
  `nested emit should not run a once listener twice, got ${nested.join(',')}`
);
assert(
  nestedEmitter.listenerCount('go') === 1,
  `once listener should be removed after firing, got ${nestedEmitter.listenerCount('go')}`
);

// removeAllListeners from inside a listener must not truncate the running dispatch
const cleared = [];
const clearEmitter = new EventEmitter();
clearEmitter.on('go', () => {
  cleared.push('first');
  clearEmitter.removeAllListeners('go');
});
clearEmitter.on('go', () => cleared.push('second'));
clearEmitter.emit('go');
assert(
  cleared.join(',') === 'first,second',
  `removeAllListeners mid-emit must not skip listeners, got ${cleared.join(',')}`
);
assert(
  clearEmitter.listenerCount('go') === 0,
  'removeAllListeners should leave no listeners behind'
);

// prependListener during a dispatch must not shift the running walk onto a
// listener it already ran, and must still land at the front for the next emit
const prepended = [];
const prependEmitter = new EventEmitter();
prependEmitter.on('go', () => {
  prepended.push('first');
  if (prependEmitter.listenerCount('go') < 3) {
    prependEmitter.prependListener('go', () => prepended.push('front'));
  }
});
prependEmitter.on('go', () => prepended.push('second'));

prependEmitter.emit('go');
assert(
  prepended.join(',') === 'first,second',
  `prepend during emit must not disturb the running dispatch, got ${prepended.join(',')}`
);

prepended.length = 0;
prependEmitter.emit('go');
assert(
  prepended.join(',') === 'front,first,second',
  `prepended listener should run first on the next emit, got ${prepended.join(',')}`
);

// listener bookkeeping must not count entries retired mid-dispatch
const counted = [];
const countEmitter = new EventEmitter();
const goesAway = () => counted.push('goes-away');
countEmitter.on('go', () => {
  countEmitter.removeListener('go', goesAway);
  counted.push(`count=${countEmitter.listenerCount('go')}`);
  counted.push(`listeners=${countEmitter.listeners('go').length}`);
});
countEmitter.on('go', goesAway);
countEmitter.emit('go');
assert(
  counted.join(',') === 'count=1,listeners=1,goes-away',
  `listenerCount/listeners must exclude a listener removed mid-emit, got ${counted.join(',')}`
);
assert(
  countEmitter.listenerCount('go') === 1,
  `retired listener should be swept after emit, got ${countEmitter.listenerCount('go')}`
);

// once listeners fired by an inner emit must not be re-run as the outer unwinds,
// while the emitter stays reusable afterwards
const reuse = [];
const reuseEmitter = new EventEmitter();
reuseEmitter.once('go', () => reuse.push('once-a'));
reuseEmitter.once('go', () => reuse.push('once-b'));
reuseEmitter.emit('go');
reuseEmitter.emit('go');
assert(reuse.join(',') === 'once-a,once-b', `once listeners should fire exactly once, got ${reuse.join(',')}`);
assert(reuseEmitter.listenerCount('go') === 0, 'spent once listeners should leave no residue');

reuseEmitter.on('go', () => reuse.push('after'));
reuseEmitter.emit('go');
assert(reuse.join(',') === 'once-a,once-b,after', `emitter should be reusable after sweeping, got ${reuse.join(',')}`);

// two prepends in one dispatch: the deferred reorder must move both and keep
// their relative order, not just the last one it happened to visit
const multi = [];
const multiEmitter = new EventEmitter();
let armed = false;
multiEmitter.on('go', () => {
  multi.push('base');
  if (armed) return;
  armed = true;
  multiEmitter.prependListener('go', () => multi.push('A'));
  multiEmitter.prependListener('go', () => multi.push('B'));
});

multiEmitter.emit('go');
multi.length = 0;
multiEmitter.emit('go');
assert(
  multi.join(',') === 'B,A,base',
  `both mid-dispatch prepends should land in front, in order, got ${multi.join(',')}`
);
assert(
  multiEmitter.listenerCount('go') === 3,
  `expected 3 listeners after reorder, got ${multiEmitter.listenerCount('go')}`
);

// symbol keys match by identity, not by value like strings
const symEvents = [];
const symEmitter = new EventEmitter();
const symA = Symbol('shared-description');
const symB = Symbol('shared-description');

symEmitter.on(symA, () => symEvents.push('A'));
symEmitter.on(symB, () => symEvents.push('B'));
symEmitter.emit(symA);
assert(symEvents.join(',') === 'A', `identical descriptions must stay distinct events, got ${symEvents.join(',')}`);
assert(symEmitter.listenerCount(symA) === 1, 'symbol listenerCount should be 1');
assert(symEmitter.eventNames().length === 2, `expected 2 symbol event names, got ${symEmitter.eventNames().length}`);

// an emitter that churns through event names must not accumulate them
const churn = new EventEmitter();
const noop = () => {};
for (let i = 0; i < 200; i++) {
  churn.on('tmp' + i, noop);
  churn.removeListener('tmp' + i, noop);
}
churn.on('kept', noop);
assert(
  churn.eventNames().join(',') === 'kept',
  `emptied event types should be reclaimed, got ${churn.eventNames().join(',')}`
);

console.log('PASS');
