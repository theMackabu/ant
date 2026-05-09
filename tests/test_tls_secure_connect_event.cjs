const net = require('node:net');
const tls = require('node:tls');
const assert = require('node:assert');

let sawError = false;
let sawSecureConnect = false;

const timeout = setTimeout(() => {
  throw new Error('tls.connect did not fail the plain TCP peer');
}, 2000);

const server = net.createServer((socket) => {
  socket.end();
});

server.listen(0, '127.0.0.1', () => {
  const address = server.address();
  const socket = tls.connect({ port: address.port, host: '127.0.0.1' });
  assert(socket instanceof tls.TLSSocket);
  assert.strictEqual(typeof socket.renegotiate, 'function');
  assert.strictEqual(socket.ref(), socket);
  assert.strictEqual(socket.unref(), socket);
  assert.strictEqual(socket.cork(), socket);
  assert.strictEqual(socket.uncork(), socket);

  socket.on('error', (err) => {
    sawError = true;
    if (sawSecureConnect) throw new Error('plain TCP peer emitted secureConnect');
    socket.destroy();
    server.close(() => {
      clearTimeout(timeout);
      console.log('tls:plain-peer-error:ok');
    });
  });
  socket.once('secureConnect', () => {
    sawSecureConnect = true;
    if (!sawError) throw new Error('tls.connect unexpectedly accepted a plain TCP peer');
  });
});
