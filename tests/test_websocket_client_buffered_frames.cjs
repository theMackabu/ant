const assert = require('node:assert');
const crypto = require('node:crypto');
const net = require('node:net');

function acceptKey(key) {
  return crypto
    .createHash('sha1')
    .update(`${key}258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)
    .digest('base64');
}

function frame(text) {
  const payload = Buffer.from(text);
  if (payload.length < 126) {
    return Buffer.concat([Buffer.from([0x81, payload.length]), payload]);
  }
  const header = Buffer.alloc(4);
  header[0] = 0x81;
  header[1] = 126;
  header.writeUInt16BE(payload.length, 2);
  return Buffer.concat([header, payload]);
}

function closeFrame() {
  return Buffer.from([0x88, 0x00]);
}

const server = net.createServer((socket) => {
  let request = '';
  socket.on('data', (chunk) => {
    request += chunk.toString('latin1');
    const end = request.indexOf('\r\n\r\n');
    if (end === -1) return;

    const key = request.match(/^Sec-WebSocket-Key: (.+)$/mi)?.[1]?.trim();
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${acceptKey(key)}\r\n` +
      '\r\n'
    );

    const split = frame('split');
    socket.write(Buffer.concat([frame('one'), frame('two')]));
    socket.write(split.subarray(0, 3));
    setTimeout(() => {
      socket.end(Buffer.concat([split.subarray(3), closeFrame(), frame('after-close')]));
    }, 0);
    socket.removeAllListeners('data');
  });
});

const timeout = setTimeout(() => {
  console.error('timed out waiting for buffered websocket messages');
  server.close(() => process.exit(1));
}, 2000);

server.listen(0, '127.0.0.1', () => {
  const { port } = server.address();
  const messages = [];
  const ws = new WebSocket(`ws://127.0.0.1:${port}/ws`);

  ws.onmessage = (event) => {
    messages.push(String(event.data));
  };

  ws.onerror = (event) => {
    clearTimeout(timeout);
    server.close(() => {
      throw new Error(`unexpected websocket error: ${event.type}`);
    });
  };

  ws.onclose = () => {
    assert.deepStrictEqual(messages, ['one', 'two', 'split']);
    clearTimeout(timeout);
    server.close(() => {
      console.log('websocket:client-buffered-frames:ok');
    });
  };
});
