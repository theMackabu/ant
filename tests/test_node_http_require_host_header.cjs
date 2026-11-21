const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

const once = (emitter, event) => new Promise(resolve => emitter.once(event, resolve));

function raw(port, text) {
  return new Promise((resolve, reject) => {
    let out = '';
    const socket = net.connect(port, '127.0.0.1', () => socket.write(text));
    socket.on('data', chunk => { out += String(chunk); });
    socket.on('close', () => resolve(out));
    socket.on('error', reject);
  });
}

async function statuses(options) {
  let hits = 0;
  const server = http.createServer(options, (req, res) => {
    hits++;
    res.end('ok');
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const { port } = server.address();

  try {
    const missing = await raw(port, 'GET / HTTP/1.1\r\nConnection: close\r\n\r\n');
    const legacy = await raw(port, 'GET / HTTP/1.0\r\n\r\n');
    const present = await raw(port, 'GET / HTTP/1.1\r\nHost: example.test\r\nConnection: close\r\n\r\n');
    return { missing, hits, lines: [missing, legacy, present].map(text => text.split('\r\n')[0]) };
  } finally {
    server.close();
  }
}

(async () => {
  const strict = await statuses({});
  assert.deepStrictEqual(strict.lines, ['HTTP/1.1 400 Bad Request', 'HTTP/1.1 200 OK', 'HTTP/1.1 200 OK']);
  assert.strictEqual(strict.hits, 2);
  assert.match(strict.missing, /\r\nconnection: close\r\n/i);

  const relaxed = await statuses({ requireHostHeader: false });
  assert.deepStrictEqual(relaxed.lines, ['HTTP/1.1 200 OK', 'HTTP/1.1 200 OK', 'HTTP/1.1 200 OK']);
  assert.strictEqual(relaxed.hits, 3);

  assert.strictEqual(http.createServer().requireHostHeader, true);
  assert.strictEqual(http.createServer({ requireHostHeader: false }).requireHostHeader, false);
  console.log('ok');
})();
