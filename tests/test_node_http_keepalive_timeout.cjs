const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

const once = (emitter, event) => new Promise(resolve => emitter.once(event, resolve));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const request = 'GET / HTTP/1.1\r\nHost: example.test\r\n\r\n';

(async () => {
  const server = http.createServer((req, res) => res.end('ok'));
  server.keepAliveTimeout = 300;
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');

  const socket = net.connect(server.address().port, '127.0.0.1');
  let responses = 0;
  let closedAt = 0;
  socket.on('data', chunk => { responses += String(chunk).split('HTTP/1.1 200').length - 1; });
  socket.on('close', () => { closedAt = Date.now(); });
  await once(socket, 'connect');

  socket.write(request);
  await sleep(150);
  socket.write(request);
  await sleep(150);
  assert.strictEqual(responses, 2);
  assert.strictEqual(closedAt, 0, 'a request inside the window keeps the connection open');

  const idleFrom = Date.now();
  await once(socket, 'close');
  const idle = Date.now() - idleFrom;
  assert.ok(idle >= 100 && idle < 4000, `closed after ${idle}ms`);

  server.close();
  console.log('ok');
})();
