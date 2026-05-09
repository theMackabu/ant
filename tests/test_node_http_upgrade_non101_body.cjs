const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

const server = net.createServer((socket) => {
  socket.once('data', () => {
    socket.write(
      'HTTP/1.1 400 Bad Request\r\n' +
      'Content-Length: 10\r\n' +
      '\r\n' +
      'hello'
    );
    setTimeout(() => {
      socket.write('world');
    }, 0);
  });
});

const timeout = setTimeout(() => {
  server.close(() => {
    throw new Error('timed out waiting for non-101 upgrade response body');
  });
}, 2000);

server.listen(0, '127.0.0.1', () => {
  const { port } = server.address();
  const chunks = [];
  const req = http.request({
    host: '127.0.0.1',
    port,
    path: '/',
    headers: {
      Connection: 'Upgrade',
      Upgrade: 'test',
    },
  });

  req.on('upgrade', () => {
    throw new Error('unexpected upgrade for non-101 response');
  });

  req.on('response', (incoming) => {
    assert.strictEqual(incoming.statusCode, 400);
    incoming.on('data', chunk => chunks.push(Buffer.from(chunk)));
    incoming.on('end', () => {
      assert.strictEqual(Buffer.concat(chunks).toString('utf8'), 'helloworld');
      if (incoming.socket && typeof incoming.socket.destroy === 'function') incoming.socket.destroy();
      clearTimeout(timeout);
      server.close(() => {
        console.log('node-http:upgrade-non-101-body:ok');
      });
    });
    incoming.resume();
  });

  req.on('error', (error) => {
    server.close(() => {
      throw error;
    });
  });

  req.end();
});
