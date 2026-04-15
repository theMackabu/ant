function assertEq(name, actual, expected) {
  if (actual !== expected) {
    throw new Error(`${name}: expected ${expected}, got ${actual}`);
  }
  console.log(`ok ${name}`);
}

class ProtoThenable {
  constructor(value) {
    this.value = value;
  }
}

ProtoThenable.prototype.then = function(resolve, _reject) {
  resolve(this.value);
};

class APIPromiseLike {
  constructor(value) {
    this.promise = Promise.resolve([{ ok: true, value }]);
  }

  then(onfulfilled, onrejected) {
    return this.promise.then(
      onfulfilled ? ([result]) => onfulfilled(result.value) : undefined,
      onrejected
    );
  }
}

(async function() {
  assertEq('await plain thenable', await {
    then(resolve, _reject) {
      resolve(42);
    }
  }, 42);

  assertEq('await prototype thenable', await new ProtoThenable(99), 99);

  assertEq('await APIPromise-like thenable', await new APIPromiseLike('ok'), 'ok');

  assertEq(
    'Promise.resolve still adopts thenables',
    await Promise.resolve(new ProtoThenable('wrapped')),
    'wrapped'
  );
})().catch((err) => {
  console.error(err);
  process.exitCode = 1;
});
