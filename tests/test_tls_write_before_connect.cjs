const assert = require('node:assert');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const tls = require('node:tls');

const fixture = (name) => fs.readFileSync(path.join(__dirname, 'fixtures', 'tls', name));
const serverOptions = { key: fixture('server-key.pem'), cert: fixture('server-cert.pem') };
const clientOptions = { host: '127.0.0.1', servername: 'localhost', ca: fixture('ca-cert.pem') };

function withServer(onClient) {
  return new Promise((resolve, reject) => {
    let received = '';
    let sawEnd = false;
    const server = tls.createServer(serverOptions, (socket) => {
      socket.on('data', (chunk) => { received += chunk; });
      socket.on('end', () => { sawEnd = true; socket.end(); });
      socket.on('error', () => {});
    });
    server.listen(0, '127.0.0.1', () => {
      const client = tls.connect({ ...clientOptions, port: server.address().port });
      const closed = new Promise((done) => client.on('close', done));
      Promise.resolve(onClient(client)).catch(reject);
      closed.then(() => new Promise((done) => setTimeout(done, 30))).then(() => {
        server.close(() => resolve({ received, sawEnd }));
      });
    });
  });
}

let finished = false;
process.on('exit', () => assert.strictEqual(finished, true, 'test stalled before finishing'));

(async () => {
  {
    const events = [];
    const { received, sawEnd } = await withServer((client) => {
      client.on('error', assert.ifError);
      client.on('secureConnect', () => { events.push('secure'); client.write('3'); });
      assert.strictEqual(client.write('1', (err) => {
        assert.ifError(err);
        events.push('cb 1');
        client.write('4');
        client.end('5');
      }), true);
      assert.strictEqual(client.write('2', (err) => { assert.ifError(err); events.push('cb 2'); }), true);
    });
    assert.strictEqual(received, '12345');
    assert.strictEqual(sawEnd, true);
    assert.strictEqual(events[0], 'secure');
    assert.ok(events.indexOf('cb 1') < events.indexOf('cb 2'));
  }

  {
    const { received, sawEnd } = await withServer((client) => {
      client.on('error', assert.ifError);
      client.end('z');
    });
    assert.strictEqual(received, 'z');
    assert.strictEqual(sawEnd, true);
  }

  {
    let secure = false;
    const { received } = await withServer((client) => {
      client.on('error', assert.ifError);
      client.on('secureConnect', () => { secure = true; });
      client.end();
    });
    assert.strictEqual(secure, true);
    assert.strictEqual(received, '');
  }

  {
    const payload = 'x'.repeat(4 * 1024 * 1024);
    const { received } = await withServer((client) => {
      client.on('error', assert.ifError);
      client.write(payload);
      client.end('y');
    });
    assert.strictEqual(received.length, payload.length + 1);
    assert.strictEqual(received.at(-1), 'y');
  }

  {
    const errors = [];
    let callbackError;
    let hadError;
    await withServer((client) => {
      client.on('error', (err) => errors.push(err));
      client.on('close', (value) => { hadError = value; });
      client.write('a', (err) => { callbackError = err; });
      client.destroy();
    });
    await new Promise((done) => setImmediate(done));
    assert.deepStrictEqual(errors, []);
    assert.strictEqual(hadError, false);
    assert.strictEqual(callbackError?.code, 'ERR_SOCKET_CLOSED_BEFORE_CONNECTION');
  }

  {
    const errors = [];
    let callbackError;
    const boom = new Error('boom');
    await withServer((client) => {
      client.on('error', (err) => errors.push(err));
      client.write('a', (err) => { callbackError = err; });
      client.destroy(boom);
    });
    await new Promise((done) => setImmediate(done));
    assert.deepStrictEqual(errors, [boom]);
    assert.ok(callbackError instanceof Error);
  }

  {
    const seen = [];
    const { received } = await withServer((client) => {
      client.on('error', assert.ifError);
      for (let i = 0; i < 64; i++) {
        const tag = { i };
        client.write(String(i % 10), () => seen.push(tag.i));
      }
      client.on('secureConnect', () => client.end());
      for (let i = 0; i < 400000; i++) ({ garbage: i, more: [i] });
    });
    assert.strictEqual(received.length, 64);
    assert.deepStrictEqual(seen, Array.from({ length: 64 }, (_, i) => i));
  }

  for (const peer of ['refused', 'plaintext']) {
    const result = await new Promise((resolve) => {
      const relay = net.createServer((socket) => {
        socket.on('error', () => {});
        socket.resume();
        socket.write('not tls at all\r\n\r\n');
        socket.end();
      });
      relay.listen(0, '127.0.0.1', () => {
        const { port } = relay.address();
        const start = () => {
          const seen = { callbacks: [], errors: 0, close: null };
          const client = tls.connect({ ...clientOptions, port });
          client.on('error', () => { seen.errors++; });
          client.on('close', (hadError) => { seen.close = hadError; });
          client.write('a', (err) => seen.callbacks.push(err));
          client.write('b', (err) => seen.callbacks.push(err));
          client.on('close', () => setTimeout(() => {
            if (relay.listening) relay.close();
            resolve(seen);
          }, 30));
        };
        if (peer === 'refused') relay.close(start);
        else start();
      });
    });
    assert.ok(result.errors >= 1, `${peer}: expected an error event`);
    assert.strictEqual(result.close, true, peer);
    assert.strictEqual(result.callbacks.length, 2, peer);
    assert.ok(result.callbacks.every((err) => err instanceof Error), peer);
  }

  finished = true;
  console.log('PASS');
})().catch((err) => {
  console.error(err);
  process.exit(1);
});
