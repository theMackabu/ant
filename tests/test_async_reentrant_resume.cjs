function assertEq(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${name}: expected ${expected}, got ${actual}`);
  }
  console.log(`ok ${name}`);
}

(async function() {
  const settled = Promise.resolve('ready');

  async function leaf(label) {
    const value = await settled;
    return `${label}:${value}`;
  }

  async function middle(label) {
    return await leaf(label);
  }

  async function top() {
    const out = [];
    out.push(await middle('a'));
    out.push(await middle('b'));
    return out.join(',');
  }

  assertEq(
    'nested async resume on settled promise',
    await top(),
    'a:ready,b:ready'
  );
})().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
