const assert = require('node:assert');

const unhandled = [];
process.on('unhandledRejection', reason => unhandled.push(reason));

function observe(promise) {
  assert(promise instanceof Promise);
  return promise.then(
    value => ({ rejected: false, value }),
    value => ({ rejected: true, value })
  );
}

function checkOutcome(method, rejected, outcome, label) {
  const expectRejection = method === 'rejects';
  assert.strictEqual(outcome.rejected, rejected !== expectRejection, label);
  if (outcome.rejected) {
    assert.strictEqual(outcome.value.message,
      expectRejection ? 'Missing expected rejection' : 'Got unwanted rejection', label);
  } else {
    assert.strictEqual(outcome.value, undefined, label);
  }
}

async function main() {
  for (const method of ['rejects', 'doesNotReject']) {
    for (const rejected of [false, true]) {
      for (const callback of [false, true]) {
        const label = `${method}, rejected=${rejected}, callback=${callback}`;
        let settle;
        const input = new Promise((resolve, reject) => {
          settle = rejected ? reject : resolve;
        });
        let settled = false;
        const observed = observe(assert[method](callback ? () => input : input)).then(outcome => {
          settled = true;
          return outcome;
        });

        await new Promise(resolve => setTimeout(resolve, 0));
        assert.strictEqual(settled, false, `${label}: settled while input was pending`);
        settle(new Error('delayed result'));
        checkOutcome(method, rejected, await observed, label);

        const alreadySettled = rejected ? Promise.reject('reason') : Promise.resolve(42);
        checkOutcome(method, rejected,
          await observe(assert[method](callback ? () => alreadySettled : alreadySettled)), label);

        checkOutcome(method, rejected, await observe(assert[method](async () => {
          await Promise.resolve();
          if (rejected) throw undefined;
          return 42;
        })), `${label}: async callback`);
      }
    }

    const reason = new Error('synchronous callback');
    const outcome = await observe(assert[method](() => { throw reason; }));
    assert.strictEqual(outcome.rejected, true);
    assert.strictEqual(outcome.value, reason);
  }

  await new Promise(resolve => setTimeout(resolve, 0));
  assert.strictEqual(unhandled.length, 0, 'input rejections must remain handled');
  console.log('assert pending rejection tests passed');
}

const deadline = setTimeout(() => {
  console.error('assert pending rejection tests timed out');
  process.exit(1);
}, 3000);
main().catch(error => {
  console.error(error.stack || error);
  process.exit(1);
}).finally(() => clearTimeout(deadline));
