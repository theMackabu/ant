const assert = require("assert");

{
  const calls = [];
  const stack = new DisposableStack();
  const first = { disposed: false };
  const second = { disposed: false };
  const third = {
    disposed: false,
    [Symbol.dispose]() {
      this.disposed = true;
      calls.push("third");
    },
  };

  assert.strictEqual(stack.adopt(first, (value) => {
    value.disposed = true;
    calls.push("first");
  }), first);

  assert.strictEqual(stack.defer(() => {
    second.disposed = true;
    calls.push("second");
  }), undefined);

  const moved = stack.move();
  assert.strictEqual(moved.use(third), third);
  let movedStackRejected = false;
  try {
    stack.defer(() => {});
  } catch (error) {
    movedStackRejected = /already disposed/.test(error.message);
  }
  assert(movedStackRejected);

  assert.strictEqual(moved.dispose(), undefined);
  assert.deepStrictEqual(calls, ["third", "second", "first"]);
  assert(first.disposed);
  assert(second.disposed);
  assert(third.disposed);
  assert.strictEqual(moved.dispose(), undefined);
}

{
  const stack = new DisposableStack();
  stack.defer(() => {
    throw new Error("first");
  });
  stack.defer(() => {
    throw new Error("second");
  });

  assert.throws(() => stack.dispose(), (error) => {
    assert(error instanceof SuppressedError);
    assert.strictEqual(error.error.message, "first");
    assert.strictEqual(error.suppressed.message, "second");
    return true;
  });
}

{
  const calls = [];
  const stack = new AsyncDisposableStack();
  const first = { disposed: false };
  const second = { disposed: false };
  const third = {
    disposed: false,
    async [Symbol.asyncDispose]() {
      this.disposed = true;
      calls.push("third");
    },
  };

  assert.strictEqual(stack.adopt(first, async (value) => {
    value.disposed = true;
    calls.push("first");
  }), first);

  assert.strictEqual(stack.defer(async () => {
    second.disposed = true;
    calls.push("second");
  }), undefined);

  const moved = stack.move();
  assert.strictEqual(moved.use(third), third);

  moved.disposeAsync().then(() => {
    assert.deepStrictEqual(calls, ["third", "second", "first"]);
    assert(first.disposed);
    assert(second.disposed);
    assert(third.disposed);
  });
}

{
  const stack = new AsyncDisposableStack();
  stack.defer(async () => {
    throw new Error("async first");
  });
  stack.defer(async () => {
    throw new Error("async second");
  });

  stack.disposeAsync().then(
    () => {
      throw new Error("disposeAsync should reject");
    },
    (error) => {
      assert(error instanceof SuppressedError);
      assert.strictEqual(error.error.message, "async first");
      assert.strictEqual(error.suppressed.message, "async second");
    }
  );
}

Promise.resolve().then(() => {
  console.log("DisposableStack tests completed!");
});
