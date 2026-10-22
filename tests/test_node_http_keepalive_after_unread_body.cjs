const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

const timeout = setTimeout(() => {
  throw new Error('second keep-alive request never got a response');
}, 5000);

const body = Buffer.alloc(32 * 1024, 0x61);
let requests = 0;

const server = http.createServer((req, res) => {
  requests++;
  res.end('ok' + requests);
});

server.listen(0, '127.0.0.1', () => {
  const socket = net.connect(server.address().port, '127.0.0.1');
  let received = '';

  const request = method => {
    const head = `${method} / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n`;
    if (method === 'GET') return head + '\r\n';
    return head + `Content-Length: ${body.length}\r\n\r\n`;
  };

  socket.on('data', chunk => {
    received += chunk.toString('latin1');
    const responses = received.split('HTTP/1.1 200').length - 1;
    if (responses === 1 && !socket.sentSecond) {
      socket.sentSecond = true;
      socket.write(request('GET'));
    }
    if (responses === 2 && received.endsWith('ok2')) {
      assert.strictEqual(requests, 2);
      clearTimeout(timeout);
      socket.destroy();
      server.close(() => console.log('ok'));
    }
  });

  socket.write(Buffer.concat([Buffer.from(request('POST')), body]));
});
