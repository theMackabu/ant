import { RpcServer } from 'ant:rpc';
import { constants } from 'ant:os';

function delay(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

const server = new RpcServer({
  add: ([a, b]) => [a + b],
  echo: ([value]) => [value],
  slow: async ([value]) => {
    await delay(25);
    return [value];
  },
  bytes: ([value]) => [value],
});

server.register('mul', ([a, b]) => [a * b]);

await server.listen({
  host: '127.0.0.1',
  port: 7000,
  workers: 0,
});

console.log(`rpc server listening on 127.0.0.1:${server.port}`);
console.log('run: ./build/ant examples/rpc/client.js');

async function close() {
  console.log('\nclosing rpc server...');
  await server.close();
  process.exit(0);
}

Ant.signal(constants.signals.SIGINT, close);
Ant.signal(constants.signals.SIGTERM, close);

