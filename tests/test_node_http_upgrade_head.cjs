const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

const server = net.createServer((socket) => {
  socket.once('data', () => {
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Connection: Upgrade\r\n' +
      'Upgrade: test\r\n' +
      '\r\n' +
      'head-bytes'
    );
  });
});

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

  req.on('upgrade', (res, socket, head) => {
    assert.strictEqual(res.statusCode, 101);
    assert.strictEqual(head.toString('utf8'), 'head-bytes');
    socket.destroy();
    server.close(() => {
      console.log('node-http:upgrade-head:ok');
    });
  });

  req.on('error', (error) => {
    server.close(() => {
      throw error;
    });
  });

  req.end();
});
