const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

let serverSocketClosed = false;
let sawClientError = false;

function finish() {
  if (!sawClientError || !serverSocketClosed) return;
  clearTimeout(timeout);
  server.close(() => {
    console.log('node-http:upgrade-max-header-size:ok');
  });
}

const server = net.createServer((socket) => {
  socket.on('close', () => {
    serverSocketClosed = true;
    finish();
  });

  socket.once('data', () => {
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      `X-Fill: ${'a'.repeat(http.maxHeaderSize)}\r\n` +
      '\r\n'
    );
  });
});

const timeout = setTimeout(() => {
  server.close(() => {
    throw new Error('timed out waiting for oversized upgrade header failure');
  });
}, 2000);

server.listen(0, '127.0.0.1', () => {
  const { port } = server.address();
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
    throw new Error('unexpected upgrade for oversized response header');
  });

  req.on('error', (error) => {
    assert.match(error.message, /maxHeaderSize|header/i);
    sawClientError = true;
    finish();
  });

  req.end();
});
