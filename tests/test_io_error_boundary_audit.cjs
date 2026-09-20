const assert = require('node:assert');
const net = require('node:net');
const tls = require('node:tls');
const dns = require('node:dns');
const { RpcServer, RpcClient } = require('ant:rpc');
const { Readable, Writable } = require('node:stream');
const { pipeline } = require('node:stream/promises');

function expectTypeError(call, message) {
  let caught = false;
  try { call(); } catch (error) {
    caught = true;
    assert(error instanceof TypeError, `${message}: expected an ordinary TypeError`);
    assert.strictEqual(error.message, message);
  }
  assert(caught, `${message}: expected a throw`);
}

async function withTimeout(promise) {
  let timer;
  try {
    return await Promise.race([
      promise,
      new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('operation did not settle')), 1000);
      })
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function expectRejection(call, expected) {
  const promise = call();
  assert(promise instanceof Promise, 'failure must return a promise');
  let caught = false;
  await withTimeout(promise.then(
    () => { throw new Error('expected rejection'); },
    error => {
      caught = true;
      if (expected) assert.strictEqual(error, expected);
      else assert(error instanceof Error, 'rejection must contain an Error object');
    }
  ));
  assert(caught);
}

async function main() {
  expectTypeError(() => net.Socket.prototype.write.call({}, 'x'), 'Invalid net.Socket');
  expectTypeError(() => net.Server.prototype.listen.call({}, 0), 'Invalid net.Server');
  expectTypeError(() => tls.TLSSocket.prototype.write.call({}, 'x'), 'Invalid TLS socket');
  expectTypeError(() => RpcServer.prototype.register.call({}, 'x', () => []), 'Invalid RpcServer');
  expectTypeError(() => RpcClient.prototype.connect.call({}), 'Invalid RpcClient');
  const rpcServer = new RpcServer();
  expectTypeError(() => rpcServer.register(1, () => []), 'route name must be a string');
  rpcServer.close();

  // A malformed numeric host fails locally without depending on external DNS.
  await expectRejection(() => dns.promises.lookup('256.256.256.256'));
  await expectRejection(() => dns.promises.lookup());
  await expectRejection(() => dns.promises.lookup(42));

  const pipeFailure = new Error('source pipe failed');
  const source = new Readable({ read() {} });
  const destination = new Writable({ write(chunk, encoding, done) { done(); } });
  source.pipe = () => { throw pipeFailure; };
  await expectRejection(() => pipeline(source, destination), pipeFailure);
  source.destroy();
  destination.destroy();

  const registrationFailure = new Error('stream listener registration failed');
  const badSource = new Readable({ read() {} });
  const sink = new Writable({ write(chunk, encoding, done) { done(); } });
  badSource.on = () => { throw registrationFailure; };
  await expectRejection(() => pipeline(badSource, sink), registrationFailure);
  badSource.destroy();
  sink.destroy();

  const listenerGetterFailure = new Error('stream listener getter failed');
  await expectRejection(() => pipeline({ get on() { throw listenerGetterFailure; } }, {}), listenerGetterFailure);

  const iterationFailure = new Error('iterator next failed');
  const cleanupFailure = new Error('iterator cleanup failed');
  const iterable = {
    [Symbol.iterator]() { return this; },
    next() { throw iterationFailure; },
    return() { throw cleanupFailure; }
  };
  await withTimeout(new Promise((resolve, reject) => {
    const readable = Readable.from(iterable);
    readable.on('error', error => {
      try { assert.strictEqual(error, iterationFailure); resolve(); } catch (failure) { reject(failure); }
    });
    readable.resume();
  }));

  const resultFailure = new Error('iterator result getter failed');
  const asyncIterable = {
    [Symbol.asyncIterator]() { return this; },
    next() { return Promise.resolve({ get done() { throw resultFailure; } }); }
  };
  await withTimeout(new Promise((resolve, reject) => {
    const readable = Readable.from(asyncIterable);
    readable.on('error', error => {
      try { assert.strictEqual(error, resultFailure); resolve(); } catch (failure) { reject(failure); }
    });
    readable.resume();
  }));

  // Continuation after handled failures catches leaked pending exceptions.
  await Promise.resolve();
  assert.strictEqual(6 * 7, 42);
  console.log('I/O error boundary audit regressions passed');
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
