import assert from 'node:assert';
import net from 'node:net';
import http from 'node:http';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const role = process.argv[2];
const once = (emitter, event) => new Promise((resolve) => emitter.once(event, resolve));

if (role === 'listen-net') {
  const server = net.createServer((socket) => socket.end(JSON.stringify(server.address())));
  server.listen({ fd: 3 }, () => console.log('ready'));
} else if (role === 'listen-http') {
  const server = http.createServer((req, res) => res.end(`http ${req.url} ${server.address().port}`));
  server.listen({ fd: 3 }, () => console.log('ready'));
} else if (role === 'socket') {
  const socket = new net.Socket({ fd: 3, readable: true, writable: true });
  socket.on('data', (chunk) => socket.end(String(chunk).toUpperCase()));
} else {
  const self = fileURLToPath(import.meta.url);
  const collect = async (stream) => {
    let out = '';
    stream.setEncoding('utf8');
    stream.on('data', (chunk) => (out += chunk));
    await once(stream, 'end');
    return out;
  };

  const withInheritedListener = async (childRole, fn) => {
    const server = net.createServer(() => assert.fail('parent accepted a connection'));
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const { port } = server.address();
    const listener = server._handle ? server._handle.fd : server;

    const child = spawn(process.execPath, [self, childRole], { stdio: ['ignore', 'pipe', 'inherit', listener] });
    server.close();
    try {
      child.stdout.setEncoding('utf8');
      assert.strictEqual(await once(child.stdout, 'data'), 'ready\n');
      await fn(port);
    } finally {
      child.kill();
      await once(child, 'exit');
    }
  };

  await withInheritedListener('listen-net', async (port) => {
    const reply = JSON.parse(await collect(net.connect(port, '127.0.0.1')));
    assert.deepStrictEqual({ ...reply }, { address: '127.0.0.1', family: 'IPv4', port });
  });

  await withInheritedListener('listen-http', async (port) => {
    const client = net.connect(port, '127.0.0.1');
    client.write('GET /fd HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n');
    const reply = await collect(client);
    assert.match(reply, /^HTTP\/1\.1 200 /);
    assert.ok(reply.includes(`http /fd ${port}`), reply);
  });

  {
    const server = net.createServer();
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');

    const client = net.connect(server.address().port, '127.0.0.1');
    const [peer] = await Promise.all([once(server, 'connection'), once(client, 'connect')]);
    server.close();

    const child = spawn(process.execPath, [self, 'socket'], { stdio: ['ignore', 'inherit', 'inherit', client] });
    client.destroy();
    peer.write('ping');
    assert.strictEqual(await collect(peer), 'PING');
    await once(child, 'exit');
  }

  {
    const first = net.createServer(() => assert.fail('original server accepted a connection'));
    first.listen(0, '127.0.0.1');
    await once(first, 'listening');
    const { port } = first.address();

    const second = net.createServer((socket) => socket.end('second'));
    second.listen(first);
    await once(second, 'listening');
    assert.strictEqual(second.address().port, port);
    assert.strictEqual(await collect(net.connect(port, '127.0.0.1')), 'second');

    second.close();
    first.close();
  }

  {
    const server = net.createServer();
    server.listen({ fd: 1 << 20 });
    const error = await once(server, 'error');
    assert.strictEqual(error.code, 'EINVAL');
    assert.strictEqual(error.syscall, 'listen');
  }

  assert.throws(() => new net.Socket({ fd: -1 }), { code: 'ERR_OUT_OF_RANGE' });
  assert.throws(() => new net.Socket({ fd: 'x' }), { code: 'ERR_INVALID_ARG_TYPE' });

  console.log('ok');
}
