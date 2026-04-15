function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function main() {
  const chunk = {
    label: 'alpha',
    fn() {
      return 'ok';
    },
  };

  const source = new ReadableStream({
    start(controller) {
      controller.enqueue(chunk);
      controller.close();
    },
  });

  const [branch1, branch2] = source.tee();
  const reader1 = branch1.getReader();
  const reader2 = branch2.getReader();

  const [{ done: done1, value: value1 }, { done: done2, value: value2 }] =
    await Promise.all([reader1.read(), reader2.read()]);

  assert(done1 === false, 'branch1 should receive a chunk');
  assert(done2 === false, 'branch2 should receive a chunk');
  assert(value1 === chunk, 'branch1 should receive the original chunk object');
  assert(value2 === chunk, 'branch2 should receive the original chunk object');
  assert(value1 === value2, 'tee branches should share the same chunk reference');
  assert(typeof value2.fn === 'function', 'function-valued chunk properties should survive tee');
  assert(value2.fn() === 'ok', 'function-valued chunk should remain callable');

  console.log('readable stream tee preserves object chunks without cloning');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
