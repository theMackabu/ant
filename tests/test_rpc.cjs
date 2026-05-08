import { RpcServer, RpcClient } from 'ant:rpc';

function assert(value, message) {
  if (!value) throw new Error(message || 'assertion failed');
}

async function assertRejects(fn, contains) {
  let rejected = false;
  try {
    await fn();
  } catch (error) {
    rejected = true;
    const message = String(error && error.message ? error.message : error);
    if (contains) assert(message.includes(contains), `expected "${message}" to include "${contains}"`);
  }
  assert(rejected, 'expected rejection');
}

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function main() {
  assert(typeof RpcServer === 'function', 'RpcServer import');
  assert(typeof RpcClient === 'function', 'RpcClient import');

  const server = new RpcServer({
    add: ([a, b]) => [a + b],
    echo: ([value]) => [value]
  });

  server.register('mul', ([a, b]) => [a * b]);
  server.register('slow', async ([value]) => {
    await delay(10);
    return [value];
  });
  server.register('text', ([value]) => [value + ' world']);
  server.register('bytes', ([value]) => [value]);
  server.register('big', ([value]) => [value + 1n]);
  server.register('throws', () => {
    throw new Error('handler exploded');
  });
  server.register('rejects', async () => {
    await delay(1);
    throw new Error('handler rejected');
  });
  server.register('badReturn', () => 42);
  server.unregister('echo');

  await server.listen({ host: '127.0.0.1', port: 0, workers: 0 });
  assert(server.port > 0, 'server.port');

  const client = new RpcClient({ host: '127.0.0.1', port: server.port });
  await client.connect();
  await client.ping();

  let response = await client.call('add', [5, 10]);
  assert(Array.isArray(response), 'call response is array');
  assert(response[0] === 15, 'sync primitive response');

  response = await client.call('mul', [6, 7]);
  assert(response[0] === 42, 'registered route');

  response = await client.call('slow', [123]);
  assert(response[0] === 123, 'async handler response');

  response = await client.call('text', ['hello']);
  assert(response[0] === 'hello world', 'string round trip');

  const bytes = new Uint8Array([1, 2, 3, 254]);
  response = await client.call('bytes', [bytes]);
  assert(response[0] instanceof Uint8Array, 'bytes response type');
  assert(response[0].length === bytes.length, 'bytes response length');
  assert(
    response[0].every((value, index) => value === bytes[index]),
    'bytes response contents'
  );

  response = await client.call('big', [9007199254740993n]);
  assert(response[0] === 9007199254740994n, 'bigint round trip');

  await assertRejects(() => client.call('missing', []), 'missing');
  await assertRejects(() => client.call('echo', ['removed']), 'echo');
  await assertRejects(() => client.call('throws', []), 'throws');
  await assertRejects(() => client.call('rejects', []), 'rejects');
  await assertRejects(() => client.call('badReturn', []), 'badReturn');

  const concurrent = await Promise.all([
    client.call('add', [1, 2]),
    client.call('slow', [4]),
    client.call('mul', [3, 5]),
    client.call('text', ['goodbye'])
  ]);

  assert(concurrent[0][0] === 3, 'concurrent add');
  assert(concurrent[1][0] === 4, 'concurrent slow');
  assert(concurrent[2][0] === 15, 'concurrent mul');
  assert(concurrent[3][0] === 'goodbye world', 'concurrent text');

  await client.close();
  await client.close();
  await server.close();
  await server.close();

  console.log('rpc tests passed');
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
