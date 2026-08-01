// A getter that returns undefined is a real result, not a lookup miss. Treating it as a
// miss made callers fall through to a property location resolved before the getter ran,
// and a getter that deletes an earlier key shifts every later slot down, so the load
// returned the neighbouring property instead.
function assert(condition, message) {
  if (!condition) { console.log('FAIL:', message); process.exit(1); }
}

// plain object: getter deletes an earlier key and returns undefined
{
  const o = {};
  Object.defineProperty(o, 'a', { value: 'A', configurable: true, enumerable: true });
  Object.defineProperty(o, 'b', {
    get() { delete o.a; return undefined; },
    configurable: true, enumerable: true,
  });
  o.c = 'C';
  assert(o.b === undefined, `object getter should yield undefined, got ${JSON.stringify(o.b)}`);
}

// array index: same shape through the dense/shape lookup path
{
  const arr = [];
  Object.defineProperty(arr, '0', { value: 'Z', configurable: true, enumerable: true });
  Object.defineProperty(arr, '1', {
    get() { delete arr[0]; return undefined; },
    configurable: true, enumerable: true,
  });
  Object.defineProperty(arr, '2', { value: 'Y', configurable: true, enumerable: true });
  assert(arr[1] === undefined, `array getter should yield undefined, got ${JSON.stringify(arr[1])}`);
}

// a getter returning undefined without touching anything is still undefined
{
  const o = { get plain() { return undefined; }, other: 'OTHER' };
  assert(o.plain === undefined, `plain getter should yield undefined, got ${JSON.stringify(o.plain)}`);
}

// prototype getters returning undefined must not fall through to a data property
{
  const proto = { get shadowed() { return undefined; } };
  const o = Object.create(proto);
  assert(o.shadowed === undefined, `proto getter should yield undefined, got ${JSON.stringify(o.shadowed)}`);
}

// getters that return real values still work, and are not confused with a miss
{
  const o = { get v() { return 42; } };
  assert(o.v === 42, `getter should yield 42, got ${o.v}`);
}

// a getter with no get function defined yields undefined
{
  const o = {};
  Object.defineProperty(o, 'setOnly', { set(v) {}, configurable: true, enumerable: true });
  assert(o.setOnly === undefined, `set-only accessor should read undefined, got ${JSON.stringify(o.setOnly)}`);
}

// throwing getters still propagate
{
  let threw = false;
  const o = { get boom() { throw new Error('boom'); } };
  try { o.boom; } catch (e) { threw = e.message === 'boom'; }
  assert(threw, 'throwing getter should propagate');
}

// includes() over an array whose elements came from a dense buffer sees accessor results
{
  const arr = [1, 2, 3];
  Object.defineProperty(arr, '1', {
    get() { return undefined; }, configurable: true, enumerable: true,
  });
  assert(arr.includes(undefined) === true, 'includes should observe the undefined getter result');
}

console.log('PASS');
