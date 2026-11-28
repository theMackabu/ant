const assert = require('node:assert/strict');
const net = require('node:net');

function frame(opcode, payload, { fin = true } = {}) {
  payload = Buffer.from(payload);
  assert(payload.length < 126);
  const key = Buffer.from([1, 2, 3, 4]);
  return Buffer.concat([
    Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | payload.length]),
    key,
    payload.map((byte, i) => byte ^ key[i % 4]),
  ]);
}

function readFrames(buffer) {
  const frames = [];
  while (buffer.length >= 2 && buffer.length >= 2 + (buffer[1] & 0x7f)) {
    const length = buffer[1] & 0x7f;
    frames.push({ opcode: buffer[0] & 0x0f, payload: buffer.subarray(2, 2 + length) });
    buffer = buffer.subarray(2 + length);
  }
  return frames;
}

const seen = [];
const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  websocket: { maxPayloadLength: 64 },
  fetch(request, ctx) {
    const { socket, response } = ctx.upgradeWebSocket(request);
    socket.onmessage = event => seen.push(`message:${event.data}`);
    socket.onerror = () => seen.push('error');
    socket.onclose = event => seen.push(`close:${event.code}:${event.reason}`);
    return response;
  },
});

async function exchange(frames) {
  seen.length = 0;
  const socket = net.createConnection({ host: '127.0.0.1', port: server.port });
  let buffer = Buffer.alloc(0);
  let upgraded = false;

  socket.on('data', data => {
    buffer = Buffer.concat([buffer, data]);
    if (upgraded) return;
    const end = buffer.indexOf('\r\n\r\n');
    if (end < 0) return;
    assert.match(buffer.subarray(0, end).toString('latin1'), /^HTTP\/1\.1 101 /);
    buffer = buffer.subarray(end + 4);
    upgraded = true;
    socket.write(Buffer.concat(frames));
  });

  socket.write(
    'GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
    'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n'
  );

  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('server kept the connection open')), 3000);
    socket.on('error', () => {});
    socket.on('close', () => {
      clearTimeout(timer);
      resolve();
    });
  });

  await new Promise(resolve => setTimeout(resolve, 20));
  return { replies: readFrames(buffer), events: seen.slice() };
}

function closeCode(reply) {
  assert.equal(reply.opcode, 8);
  return reply.payload.readUInt16BE(0);
}

(async () => {
  let result = await exchange([
    frame(1, 'x'.repeat(63), { fin: false }),
    frame(9, 'ok'),
    frame(0, 'y'),
    frame(8, [0x03, 0xe8]),
  ]);
  assert.equal(result.replies.length, 2);
  assert.equal(result.replies[0].opcode, 10);
  assert.equal(result.replies[0].payload.toString(), 'ok');
  assert.equal(closeCode(result.replies[1]), 1000);
  assert.deepEqual(result.events, [`message:${'x'.repeat(63)}y`, 'close:1000:']);

  result = await exchange([frame(1, 'x'.repeat(63), { fin: false }), frame(0, 'yy')]);
  assert.equal(result.replies.length, 1);
  assert.equal(closeCode(result.replies[0]), 1009);
  assert.equal(result.events.some(event => event.startsWith('message:')), false);

  result = await exchange([frame(1, [0xff])]);
  assert.equal(result.replies.length, 1);
  assert.equal(closeCode(result.replies[0]), 1007);
  assert.equal(result.events.some(event => event.startsWith('message:')), false);

  result = await exchange([frame(3, 'z'), frame(1, 'after-reserved')]);
  assert.equal(result.events.includes('error'), true);
  assert.equal(result.events.some(event => event.startsWith('message:')), false);

  server.stop();
  console.log('websocket:server-frames:ok');
})().catch(error => {
  console.error(error);
  process.exit(1);
});
