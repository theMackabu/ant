async function delayed(val) {
  return val;
}

function add(a, b, c) {
  return a + b + c;
}

async function run() {
  // 1: basic inline await in call args
  console.log('=== Test 1: inline await in call args ===');
  const r1 = add(await delayed(1), 2, 3);
  console.log(r1 === 6 ? '✓' : '✗', 'add(await delayed(1), 2, 3) =', r1);

  // 2: multiple inline awaits
  console.log('\n=== Test 2: multiple inline awaits ===');
  const r2 = add(await delayed(10), await delayed(20), await delayed(30));
  console.log(r2 === 60 ? '✓' : '✗', 'add(await, await, await) =', r2);

  // 3: inline await as method call arg
  console.log('\n=== Test 3: method call with inline await ===');
  const obj = { greet(name) { return 'hello ' + name; } };
  const r3 = obj.greet(await delayed('world'));
  console.log(r3 === 'hello world' ? '✓' : '✗', 'obj.greet(await) =', r3);

  // 4: chained property method with inline await
  console.log('\n=== Test 4: chained method with inline await ===');
  const nested = { inner: { say(msg) { return 'said: ' + msg; } } };
  const r4 = nested.inner.say(await delayed('hi'));
  console.log(r4 === 'said: hi' ? '✓' : '✗', 'nested.inner.say(await) =', r4);

  // 5: inline await with async fs (the real-world case)
  console.log('\n=== Test 5: console.log with inline await ===');
  console.log('value:', await delayed(42));

  // 6: array push with inline await
  console.log('\n=== Test 6: array method with inline await ===');
  const arr = [1, 2];
  arr.push(await delayed(3));
  console.log(arr.length === 3 && arr[2] === 3 ? '✓' : '✗', 'arr.push(await) =', arr);

  // 7: inline await with real fs stream
  console.log('\n=== Test 7: inline await stream (real fs) ===');
  const { stream } = await import('ant:fs');
  const file = import.meta.dirname + '/test_inline_await.js';
  const data = await stream(file);
  console.log(data.length > 0 ? '✓' : '✗', 'assigned await stream:', data.length, 'bytes');

  // 8: inline await stream as function arg
  console.log('\n=== Test 8: fn(await stream(...)) ===');
  function getLen(s) { return s.length; }
  const r8 = getLen(await stream(file));
  console.log(r8 > 0 ? '✓' : '✗', 'getLen(await stream()) =', r8);

  // 9: inline await stream as method arg
  console.log('\n=== Test 9: obj.method(await stream(...)) ===');
  const helper = { size(s) { return s.length; } };
  const r9 = helper.size(await stream(file));
  console.log(r9 > 0 ? '✓' : '✗', 'helper.size(await stream()) =', r9);

  // 10: chained method with inline await stream
  console.log('\n=== Test 10: a.b.method(await stream(...)) ===');
  const deep = { util: { size(s) { return s.length; } } };
  const r10 = deep.util.size(await stream(file));
  console.log(r10 > 0 ? '✓' : '✗', 'deep.util.size(await stream()) =', r10);

  console.log('\n✓ done');
}

run();
