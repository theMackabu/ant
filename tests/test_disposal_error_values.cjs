const assert = require('node:assert');
function throwsExactly(fn, reason) {
  let caught = false;
  try { fn(); } catch (error) { caught = true; assert.strictEqual(error, reason); }
  assert.ok(caught, 'missing disposal throw');
}
async function rejectsExactly(promise, reason) {
  let caught = false;
  try { await promise; } catch (error) { caught = true; assert.strictEqual(error, reason); }
  assert.ok(caught, 'missing rejection');
}
async function main() {
  for (const reason of [undefined, null, new Error('disposal')]) {
    await rejectsExactly(Promise.resolve().then(() => { throw reason; }), reason);
    await rejectsExactly(Promise.try(() => { throw reason; }), reason);
    for (const stack of [new DisposableStack(), new AsyncDisposableStack()]) {
      stack.defer(() => { throw reason; });
      if (stack instanceof DisposableStack) throwsExactly(() => stack.dispose(), reason);
      else await rejectsExactly(stack.disposeAsync(), reason);
    }
    throwsExactly(() => { using resource = { [Symbol.dispose]() { throw reason; } }; }, reason);
    await rejectsExactly((async () => { await using resource = { [Symbol.asyncDispose]() { throw reason; } }; })(), reason);
    await rejectsExactly((async () => { await using resource = { [Symbol.asyncDispose]() { return Promise.reject(reason); } }; })(), reason);

    const sync = { get [Symbol.dispose]() { throw reason; } };
    const async = { get [Symbol.asyncDispose]() { throw reason; }, get [Symbol.dispose]() { throw new Error('unexpected fallback'); } };
    throwsExactly(() => { using resource = sync; }, reason);
    throwsExactly(() => new DisposableStack().use(sync), reason);
    throwsExactly(() => new AsyncDisposableStack().use(async), reason);
    throwsExactly(() => new AsyncDisposableStack().use(sync), reason);
    await rejectsExactly((async () => { await using resource = async; })(), reason);
    await rejectsExactly((async () => { await using resource = sync; })(), reason);
  }
  for (const stack of [new DisposableStack(), new AsyncDisposableStack()]) {
    stack.defer(() => { throw 'second'; });
    stack.defer(() => { throw undefined; });
    let caught;
    try { if (stack instanceof DisposableStack) stack.dispose(); else await stack.disposeAsync(); }
    catch (error) { caught = error; }
    assert.ok(caught instanceof SuppressedError);
    assert.strictEqual(caught.error, 'second');
    assert.strictEqual(caught.suppressed, undefined);
  }
  let caught;
  try { using resource = { [Symbol.dispose]() { throw 'dispose'; } }; throw undefined; }
  catch (error) { caught = error; }
  assert.ok(caught instanceof SuppressedError);
  assert.strictEqual(caught.error, 'dispose');
  assert.strictEqual(caught.suppressed, undefined);
  caught = undefined;
  try { await (async () => {
    await using resource = { [Symbol.asyncDispose]() { throw 'async dispose'; } };
    throw undefined;
  })(); } catch (error) { caught = error; }
  assert.ok(caught instanceof SuppressedError);
  assert.strictEqual(caught.error, 'async dispose');
  assert.strictEqual(caught.suppressed, undefined);
  await rejectsExactly((async () => { await using resource = null; throw undefined; })(), undefined);
  console.log('Disposal error values ok');
}
main().catch(error => { console.error(error); process.exit(1); });
