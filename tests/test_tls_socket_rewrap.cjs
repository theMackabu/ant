const assert = require('node:assert');
const net = require('node:net');
const tls = require('node:tls');

const timeout = setTimeout(() => {
  throw new Error('re-wrapped TLS socket never closed');
}, 5000);

const server = net.createServer(peer => {
  peer.on('error', () => {});
  peer.end('this is not a TLS handshake\r\n');
});

server.listen(0, '127.0.0.1', () => {
  const socket = tls.connect({ port: server.address().port, host: '127.0.0.1', rejectUnauthorized: false });
  const wrapped = new tls.TLSSocket(socket);
  let errored = false;

  socket.on('error', () => { errored = true; });
  wrapped.on('error', () => {});
  socket.on('data', () => {});

  socket.on('close', () => {
    const junk = [];
    for (let i = 0; i < 20000; i++) junk.push({ i, s: 'x' + i });

    assert.strictEqual(errored, true);
    assert.strictEqual(socket.destroyed, true);
    assert.strictEqual(socket._readableState.destroyed, true);
    assert.ok(!socket.readable);

    clearTimeout(timeout);
    server.close(() => console.log('ok'));
  });
});
