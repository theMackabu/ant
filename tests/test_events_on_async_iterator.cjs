const { on, EventEmitter } = require('events');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function main() {
  // events delivered while awaiting, then 'error' rejects — after the buffer drains
  const ee = new EventEmitter();
  setTimeout(() => { ee.emit('x', 1, 2); ee.emit('x', 3); ee.emit('error', new Error('boom')); }, 5);
  const got = [];
  try {
    for await (const args of on(ee, 'x')) got.push(args.join(','));
  } catch (error) {
    got.push('err:' + error.message);
  }
  assert(
    got.join('|') === '1,2|3|err:boom',
    `buffered events must drain before the error surfaces, got ${got.join('|')}`
  );

  // events emitted before the first next() are buffered, not lost
  const buf = new EventEmitter();
  const it = on(buf, 'y');
  buf.emit('y', 'a');
  buf.emit('y', 'b');
  assert((await it.next()).value[0] === 'a', 'first buffered event');
  assert((await it.next()).value[0] === 'b', 'second buffered event');

  // breaking out of the loop detaches both the event and error listeners
  const br = new EventEmitter();
  setTimeout(() => br.emit('z', 1), 5);
  for await (const _ of on(br, 'z')) break;
  assert(br.listenerCount('z') === 0, 'event listener should detach on break');
  assert(br.listenerCount('error') === 0, 'error listener should detach on break');

  // an abort signal rejects the pending next()
  const ac = new AbortController();
  const sig = new EventEmitter();
  setTimeout(() => ac.abort(), 5);
  let aborted = null;
  try {
    for await (const _ of on(sig, 'w', { signal: ac.signal })) {}
  } catch (error) {
    aborted = error && error.name;
  }
  assert(aborted === 'AbortError', `expected AbortError, got ${aborted}`);

  console.log('PASS');
}

main().catch(error => {
  console.log('FAIL:', error && error.message);
  process.exit(1);
});
