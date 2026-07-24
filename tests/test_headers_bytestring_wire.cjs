const assert = require('node:assert');
const net = require('node:net');

function withTimeout(promise, label) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error(`${label} timed out`)),
      3000,
    );
    promise.then(
      value => {
        clearTimeout(timer);
        resolve(value);
      },
      error => {
        clearTimeout(timer);
        reject(error);
      },
    );
  });
}

async function captureOutgoingHeader() {
  const server = Ant.serve({
    hostname: '127.0.0.1',
    port: 0,
    fetch() {
      return new Response('ok', { headers: { 'x-byte': '\u00e9' } });
    },
  });

  try {
    return await withTimeout(new Promise((resolve, reject) => {
      const chunks = [];
      const socket = net.createConnection({
        host: '127.0.0.1',
        port: server.port,
      });
      socket.on('connect', () => {
        socket.write('GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n');
      });
      socket.on('data', chunk => chunks.push(Buffer.from(chunk)));
      socket.on('error', reject);
      socket.on('end', () => resolve(Buffer.concat(chunks)));
    }), 'outgoing header');
  } finally {
    server.stop();
  }
}

async function readIncomingHeader() {
  let requestBytes = null;
  const server = net.createServer(socket => {
    const chunks = [];
    socket.on('data', chunk => {
      chunks.push(Buffer.from(chunk));
      const request = Buffer.concat(chunks);
      if (request.indexOf(Buffer.from('\r\n\r\n', 'ascii')) === -1) return;
      requestBytes = request;
      socket.end(Buffer.concat([
        Buffer.from('HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-Byte: ', 'ascii'),
        Buffer.from([0xe9]),
        Buffer.from('\r\nConnection: close\r\n\r\n', 'ascii'),
      ]));
    });
  });

  await withTimeout(new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  }), 'incoming server listen');

  try {
    const { port } = server.address();
    const response = await withTimeout(
      fetch(`http://127.0.0.1:${port}/`, {
        headers: { 'x-byte': '\u00e9' },
      }),
      'incoming fetch',
    );
    return {
      value: response.headers.get('x-byte'),
      request: requestBytes,
    };
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
}

async function main() {
  const incoming = await readIncomingHeader();
  assert.equal(incoming.value, '\u00e9');
  const requestMarker = Buffer.from('x-byte: ', 'ascii');
  const requestMarkerIndex = incoming.request.indexOf(requestMarker);
  assert.notEqual(requestMarkerIndex, -1, 'outgoing request omitted x-byte');
  assert.equal(incoming.request[requestMarkerIndex + requestMarker.length], 0xe9);
  assert.equal(incoming.request[requestMarkerIndex + requestMarker.length + 1], 0x0d);

  const response = await captureOutgoingHeader();
  const marker = Buffer.from('x-byte: ', 'ascii');
  const markerIndex = response.indexOf(marker);
  assert.notEqual(markerIndex, -1, 'outgoing response omitted x-byte');
  assert.equal(response[markerIndex + marker.length], 0xe9);
  assert.equal(response[markerIndex + marker.length + 1], 0x0d);

  console.log('headers:bytestring-wire:ok');
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
