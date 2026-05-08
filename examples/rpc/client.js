import { RpcClient } from 'ant:rpc';

const host = process.argv[2] || '127.0.0.1';
const port = Number(process.argv[3] || 7000);

const client = new RpcClient({ host, port });

await client.connect();

try {
  const [sum] = await client.call('add', [5, 10]);
  console.log('add(5, 10) =', sum);

  const [product] = await client.call('mul', [6, 7]);
  console.log('mul(6, 7) =', product);

  const [message] = await client.call('echo', ['hello from ant rpc']);
  console.log('echo =', message);

  const [slow] = await client.call('slow', ['done after a short delay']);
  console.log('slow =', slow);

  const bytes = new Uint8Array([1, 2, 3, 254]);
  const [roundTrip] = await client.call('bytes', [bytes]);
  console.log('bytes =', Array.from(roundTrip).join(', '));

  await client.ping();
  console.log('ping ok');
} finally {
  await client.close();
}
