const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

let observedHostHeader;

const server = net.createServer((socket) => {
  socket.once('data', (chunk) => {
    const request = chunk.toString('latin1');
    const match = /^Host: ([^\r\n]*)/im.exec(request);
    observedHostHeader = match ? match[1] : undefined;

    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Connection: Upgrade\r\n' +
      'Upgrade: test\r\n' +
      '\r\n'
    );
  });
});

server.listen(0, '::1', () => {
  const { port } = server.address();
  const req = http.request({
    host: '::1',
    port,
    path: '/',
    headers: {
      Connection: 'Upgrade',
      Upgrade: 'test',
    },
  });

  req.on('upgrade', (_res, socket) => {
    assert.strictEqual(observedHostHeader, `[::1]:${port}`);
    socket.destroy();
    server.close(() => {
      console.log('node-http:ipv6-host-header:ok');
    });
  });

  req.on('error', (error) => {
    server.close(() => {
      throw error;
    });
  });

  req.end();
});
