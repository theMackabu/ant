const net = require('node:net');
const tls = require('node:tls');
const assert = require('node:assert');

let sawError = false;
let sawSecureConnect = false;

const timeout = setTimeout(() => {
  throw new Error('tls.connect did not fail the plain TCP peer');
}, 2000);

const context = tls.createSecureContext();
const constructedContext = new tls.SecureContext();
const detachedSocket = new tls.TLSSocket();

assert.strictEqual(tls.default, tls);
assert.strictEqual(tls.isSecureContext(context), true);
assert.strictEqual(tls.isSecureContext(constructedContext), true);
assert.strictEqual(tls.createContext, undefined);
assert.strictEqual(tls.isContext, undefined);
assert.strictEqual(tls.checkServerIdentity('localhost', {}), undefined);
assert.deepStrictEqual(tls.getCiphers(), []);
assert.deepStrictEqual(tls.rootCertificates, []);
assert.strictEqual(Object.isFrozen(tls.rootCertificates), true);
assert.strictEqual(tls.DEFAULT_ECDH_CURVE, 'auto');
assert.strictEqual(tls.DEFAULT_MIN_VERSION, 'TLSv1.2');
assert.strictEqual(tls.DEFAULT_MAX_VERSION, 'TLSv1.3');
assert.strictEqual(tls.CLIENT_RENEG_LIMIT, 3);
assert.strictEqual(tls.CLIENT_RENEG_WINDOW, 600);
assert(detachedSocket instanceof tls.TLSSocket);
assert(detachedSocket instanceof net.Socket);
assert.strictEqual(detachedSocket.encrypted, true);
assert.strictEqual(detachedSocket.renegotiate({}, (err) => assert.strictEqual(err, null)), true);

const server = net.createServer((socket) => {
  socket.end();
});

server.listen(0, '127.0.0.1', () => {
  const address = server.address();
  const secureContext = tls.createSecureContext();
  const socket = tls.connect({
    port: address.port,
    host: '127.0.0.1',
    secureContext,
  });
  assert.strictEqual(tls.isSecureContext(secureContext), true);
  assert.strictEqual(secureContext.close(), secureContext);
  assert.strictEqual(tls.isSecureContext(secureContext), false);
  assert(socket instanceof tls.TLSSocket);
  assert(socket instanceof net.Socket);
  assert.strictEqual(typeof socket.renegotiate, 'function');
  assert.strictEqual(socket.ref(), socket);
  assert.strictEqual(socket.unref(), socket);
  assert.strictEqual(socket.cork(), socket);
  assert.strictEqual(socket.uncork(), socket);
  assert.strictEqual(socket.setEncoding('utf8'), socket);
  const badEncoding = {
    toString() {
      throw new Error('encoding coercion failed');
    },
  };
  let sawEncodingError = false;
  try {
    socket.setEncoding(badEncoding);
  } catch (err) {
    sawEncodingError = true;
    assert.match(err.message, /encoding coercion failed/);
  }
  assert.strictEqual(sawEncodingError, true);
  assert.strictEqual(socket.setEncoding(), socket);

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
