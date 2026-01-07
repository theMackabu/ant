import { test, testDeep, summary } from './helpers.js';

console.log('Navigator Tests\n');

test('navigator exists', typeof navigator, 'object');
test('navigator toStringTag', Object.prototype.toString.call(navigator), '[object Navigator]');

test('navigator.hardwareConcurrency exists', typeof navigator.hardwareConcurrency, 'number');
test('navigator.hardwareConcurrency is positive', navigator.hardwareConcurrency >= 1, true);
test('navigator.hardwareConcurrency is integer', Number.isInteger(navigator.hardwareConcurrency), true);

test('navigator.language exists', typeof navigator.language, 'string');
test('navigator.language is en-US', navigator.language, 'en-US');

test('navigator.languages exists', Array.isArray(navigator.languages), true);
test('navigator.languages length', navigator.languages.length >= 1, true);
test('navigator.languages contains language', navigator.languages.includes(navigator.language), true);

test('navigator.platform exists', typeof navigator.platform, 'string');
test('navigator.platform is non-empty', navigator.platform.length > 0, true);

test('navigator.userAgent exists', typeof navigator.userAgent, 'string');
test('navigator.userAgent starts with Ant', navigator.userAgent.startsWith('Ant/'), true);

console.log('\nLockManager Tests\n');

test('navigator.locks exists', typeof navigator.locks, 'object');
test('navigator.locks toStringTag', Object.prototype.toString.call(navigator.locks), '[object LockManager]');
test('navigator.locks.request exists', typeof navigator.locks.request, 'function');
test('navigator.locks.query exists', typeof navigator.locks.query, 'function');

(async () => {
  let exclusiveLockAcquired = false;
  let exclusiveLockName = null;
  let exclusiveLockMode = null;
  let exclusiveLockReleased = false;

  await navigator.locks
    .request('test_resource', async lock => {
      exclusiveLockAcquired = true;
      exclusiveLockName = lock.name;
      exclusiveLockMode = lock.mode;
      return 'done';
    })
    .then(() => {
      exclusiveLockReleased = true;
    });

  test('exclusive lock was acquired', exclusiveLockAcquired, true);
  test('exclusive lock has correct name', exclusiveLockName, 'test_resource');
  test('exclusive lock has correct mode', exclusiveLockMode, 'exclusive');
  test('exclusive lock was released', exclusiveLockReleased, true);

  let sharedLockMode = null;

  await navigator.locks.request('shared_resource', { mode: 'shared' }, async lock => {
    sharedLockMode = lock.mode;
  });

  test('shared lock has correct mode', sharedLockMode, 'shared');

  let ifAvailableResult = null;

  await navigator.locks.request('available_resource', { ifAvailable: true }, async lock => {
    ifAvailableResult = lock ? 'acquired' : 'not available';
  });

  test('ifAvailable lock was acquired', ifAvailableResult, 'acquired');

  const queryResult = await navigator.locks.query();

  test('query returns object', typeof queryResult, 'object');
  test('query has held array', Array.isArray(queryResult.held), true);
  test('query has pending array', Array.isArray(queryResult.pending), true);

  let lockOrder = [];

  await navigator.locks.request('order_test', async lock => {
    lockOrder.push(1);
  });

  await navigator.locks.request('order_test', async lock => {
    lockOrder.push(2);
  });

  testDeep('locks execute in order', lockOrder, [1, 2]);

  let lockToStringTag = null;
  await navigator.locks.request('tostring_test', async lock => {
    lockToStringTag = Object.prototype.toString.call(lock);
  });

  test('Lock toStringTag', lockToStringTag, '[object Lock]');

  summary();
})();
