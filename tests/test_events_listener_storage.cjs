// listener storage behaviors that the emit fast paths must preserve

const { EventEmitter, getEventListeners } = require('events');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function noop() {}

// removeListener accepts the once wrapper returned by rawListeners()
{
  const ee = new EventEmitter();
  ee.once('x', noop);
  const raw = ee.rawListeners('x');
  assert(raw.length === 1 && raw[0] !== noop, 'rawListeners should expose a once wrapper');
  assert(raw[0].listener === noop, 'once wrapper should expose .listener');
  assert(ee.rawListeners('x')[0] === raw[0], 'once wrapper identity should be stable');
  assert(ee.listeners('x')[0] === noop, 'listeners() should unwrap once listeners');
  ee.removeListener('x', raw[0]);
  assert(ee.listenerCount('x') === 0, 'removing by wrapper should remove the once listener');
  assert(ee.eventNames().length === 0, 'eventNames() should skip emptied events');
}

// removeListener removes the most recently added duplicate, like Node
{
  const ee = new EventEmitter();
  const order = [];
  const a = () => order.push('a');
  ee.on('x', a);
  ee.once('x', a);
  ee.removeListener('x', a);
  ee.emit('x');
  ee.emit('x');
  assert(order.join() === 'a,a', `expected persistent listener to remain, got ${order.join()}`);
}

// listeners() returns a fresh, exactly-sized array every call
{
  const ee = new EventEmitter();
  ee.setMaxListeners(100);
  for (let i = 0; i < 30; i++) ee.on('x', noop);
  const one = ee.listeners('x');
  const two = ee.listeners('x');
  assert(one !== two, 'listeners() must return a new array');
  assert(one.length === 30 && two.length === 30, 'listeners() length');
  one.push(1);
  assert(ee.listenerCount('x') === 30, 'mutating the result must not affect the emitter');
  assert(ee.listeners('missing').length === 0, 'unknown event gives an empty array');
}

// emptied event types are reused and keep working across on/off churn
{
  const ee = new EventEmitter();
  let hits = 0;
  const h = () => hits++;
  for (let i = 0; i < 1000; i++) {
    ee.on('x', h);
    ee.emit('x');
    ee.removeListener('x', h);
    ee.emit('x');
  }
  assert(hits === 1000, `expected 1000 hits, got ${hits}`);
  assert(ee.eventNames().length === 0, 'no live events after churn');
}

// equal keys from different call sites resolve to the same event
{
  const ee = new EventEmitter();
  let hits = 0;
  ee.on('fo' + 'o', () => hits++);
  const key = ['f', 'o', 'o'].join('');
  for (let i = 0; i < 100; i++) { ee.emit('foo'); ee.emit(key); }
  assert(hits === 200, `expected 200 hits, got ${hits}`);
}

// once listeners fire once even when the emitter is hot
{
  const ee = new EventEmitter();
  let hits = 0;
  for (let i = 0; i < 100000; i++) ee.once('x', () => hits++).emit('x');
  assert(hits === 100000, `expected 100000 once hits, got ${hits}`);
  assert(ee.listenerCount('x') === 0, 'once listeners should be consumed');
}

// plain constructors and subclasses work with getEventListeners()
{
  class Sub extends EventEmitter {}
  const sub = new Sub();
  sub.on('x', noop);
  assert(getEventListeners(sub, 'x')[0] === noop, 'getEventListeners on a subclass');
  assert(getEventListeners(new EventEmitter(), 'x').length === 0, 'getEventListeners on an empty emitter');
  const target = {};
  EventEmitter.call(target);
  assert(target !== undefined, 'EventEmitter.call on an object receiver');
}

// listener exceptions propagate out of emit and leave the emitter usable
{
  const ee = new EventEmitter();
  let ok = 0;
  ee.on('x', (v) => { if (v % 97 === 0) throw new Error('boom'); ok++; });
  let caught = 0;
  for (let i = 1; i <= 10000; i++) {
    try { ee.emit('x', i); } catch (e) { caught++; }
  }
  assert(caught === 103 && ok === 10000 - 103, `caught ${caught}, ok ${ok}`);
}

console.log('test_events_listener_storage: ok');
