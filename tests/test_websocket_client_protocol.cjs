const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const net = require('node:net');

function frame(opcode, payload, { fin = true, rsv1 = false, mask = false } = {}) {
  payload = Buffer.from(payload);
  const head = [(fin ? 0x80 : 0) | (rsv1 ? 0x40 : 0) | opcode];
  const maskBit = mask ? 0x80 : 0;
  if (payload.length < 126) head.push(maskBit | payload.length);
  else if (payload.length < 65536) head.push(maskBit | 126, payload.length >> 8, payload.length & 255);
  else head.push(maskBit | 127, 0, 0, 0, 0, payload.length >>> 24, (payload.length >>> 16) & 255, (payload.length >>> 8) & 255, payload.length & 255);
  if (!mask) return Buffer.concat([Buffer.from(head), payload]);
  const key = Buffer.from([1, 2, 3, 4]);
  return Buffer.concat([Buffer.from(head), key, payload.map((byte, i) => byte ^ key[i % 4])]);
}

function close(code, reason = '') {
  return frame(8, Buffer.concat([Buffer.from([code >> 8, code & 255]), Buffer.from(reason)]));
}

function readFrames(buffer, out) {
  for (;;) {
    if (buffer.length < 2) return buffer;
    const masked = (buffer[1] & 0x80) !== 0;
    let length = buffer[1] & 0x7f;
    let pos = 2;
    if (length === 126) {
      if (buffer.length < 4) return buffer;
      length = buffer.readUInt16BE(2);
      pos = 4;
    }
    const start = pos + (masked ? 4 : 0);
    if (buffer.length < start + length) return buffer;
    let payload = buffer.subarray(start, start + length);
    if (masked) {
      const key = buffer.subarray(pos, pos + 4);
      payload = Buffer.from(payload.map((byte, i) => byte ^ key[i % 4]));
    }
    out.push({ opcode: buffer[0] & 0x0f, masked, payload: payload.toString('hex') });
    buffer = buffer.subarray(start + length);
  }
}

async function run({ chunks = [], endAfter = 100, echoClose = false, onOpen = null }) {
  const events = [];
  const fromClient = [];

  const server = net.createServer(socket => {
    let buffer = Buffer.alloc(0);
    let upgraded = false;
    socket.on('error', () => {});
    socket.on('data', async data => {
      buffer = Buffer.concat([buffer, data]);
      if (!upgraded) {
        const end = buffer.indexOf('\r\n\r\n');
        if (end < 0) return;
        const key = /sec-websocket-key: *(\S+)/i.exec(buffer.subarray(0, end).toString('latin1'))[1];
        const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        buffer = buffer.subarray(end + 4);
        upgraded = true;
        socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
        await new Promise(resolve => setTimeout(resolve, 20));
        for (const chunk of chunks) {
          if (socket.destroyed) return;
          socket.write(chunk);
          await new Promise(resolve => setTimeout(resolve, 2));
        }
        if (endAfter !== null) setTimeout(() => socket.end(), endAfter);
      }
      const before = fromClient.length;
      buffer = readFrames(buffer, fromClient);
      const received = fromClient.slice(before).find(entry => entry.opcode === 8);
      if (!received) return;
      if (echoClose) socket.write(frame(8, Buffer.from(received.payload, 'hex')));
      socket.end();
    });
  });

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const ws = new WebSocket(`ws://127.0.0.1:${server.address().port}`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    events.push('open');
    if (onOpen) onOpen(ws);
  };
  ws.onmessage = event => {
    events.push(typeof event.data === 'string' ? `text:${event.data}` : `binary:${Buffer.from(event.data).toString('hex')}`);
  };
  ws.onerror = () => events.push('error');

  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`no close event, saw ${JSON.stringify(events)}`)), 3000);
    ws.onclose = event => {
      clearTimeout(timer);
      events.push(`close:${event.code}:${event.reason}:${event.wasClean}`);
      resolve();
    };
  });

  await new Promise(resolve => setTimeout(resolve, 30));
  await new Promise(resolve => server.close(resolve));
  assert.equal(ws.readyState, WebSocket.CLOSED);
  return { events, fromClient };
}

const unclean = ['error', 'close:1006::false'];

function closeCodeSent(fromClient) {
  const sent = fromClient.filter(entry => entry.opcode === 8);
  assert.equal(sent.length, 1);
  assert.equal(sent[0].masked, true);
  return parseInt(sent[0].payload.slice(0, 4), 16);
}

(async () => {
  let result = await run({ chunks: [frame(1, 'hi')] });
  assert.deepEqual(result.events, ['open', 'text:hi', ...unclean]);
  assert.deepEqual(result.fromClient, []);

  result = await run({ chunks: [frame(1, 'hi'), close(1000)] });
  assert.deepEqual(result.events, ['open', 'text:hi', 'close:1000::true']);
  assert.deepEqual(result.fromClient, [{ opcode: 8, masked: true, payload: '03e8' }]);

  result = await run({ chunks: [frame(9, 'p')] });
  assert.deepEqual(result.events, ['open', ...unclean]);
  assert.deepEqual(result.fromClient, [{ opcode: 10, masked: true, payload: '70' }]);

  result = await run({ chunks: [frame(1, [0xff])] });
  assert.deepEqual(result.events, ['open', ...unclean]);
  assert.equal(closeCodeSent(result.fromClient), 1007);

  result = await run({ chunks: [frame(1, [0xe2, 0x82], { fin: false }), frame(0, [0x28])] });
  assert.deepEqual(result.events, ['open', ...unclean]);
  assert.equal(closeCodeSent(result.fromClient), 1007);

  for (const chunks of [
    [frame(3, 'x'), frame(1, 'after')],
    [frame(11, 'x'), frame(1, 'after')],
    [frame(1, 'masked', { mask: true })],
    [frame(1, 'x', { rsv1: true })],
    [frame(0, 'x')],
    [frame(1, 'a', { fin: false }), frame(1, 'b')],
    [frame(9, 'a', { fin: false })],
    [frame(8, [3])],
    [close(999)],
    [close(1005)],
  ]) {
    result = await run({ chunks });
    assert.deepEqual(result.events, ['open', ...unclean]);
    assert.equal(closeCodeSent(result.fromClient), 1002);
  }

  result = await run({ chunks: [frame(1, 'ab', { fin: false }), frame(9, 'mid'), frame(0, 'cd'), close(1000, 'bye')] });
  assert.deepEqual(result.events, ['open', 'text:abcd', 'close:1000:bye:true']);
  assert.deepEqual(result.fromClient, [
    { opcode: 10, masked: true, payload: '6d6964' },
    { opcode: 8, masked: true, payload: '03e8' },
  ]);

  result = await run({
    chunks: [Buffer.concat([frame(2, 'a', { fin: false }), frame(0, 'b', { fin: false }), frame(0, 'c'), close(1001, 'away')])],
  });
  assert.deepEqual(result.events, ['open', 'binary:616263', 'close:1001:away:true']);

  result = await run({ chunks: [frame(1, [0xe2, 0x82], { fin: false }), frame(0, [0xac]), close(1000)] });
  assert.deepEqual(result.events, ['open', 'text:€', 'close:1000::true']);

  result = await run({ chunks: [frame(8, '')] });
  assert.deepEqual(result.events, ['open', 'close:1005::true']);
  assert.deepEqual(result.fromClient, [{ opcode: 8, masked: true, payload: '' }]);

  result = await run({ chunks: [close(4999, 'custom')] });
  assert.deepEqual(result.events, ['open', 'close:4999:custom:true']);

  result = await run({ chunks: [Buffer.concat([frame(1, 'one'), close(1000), frame(1, 'late')])] });
  assert.deepEqual(result.events, ['open', 'text:one', 'close:1000::true']);

  const big = frame(2, Buffer.alloc(70000, 0x61));
  result = await run({ chunks: [big.subarray(0, 5), big.subarray(5, 30000), big.subarray(30000), close(1000)] });
  assert.equal(result.events.length, 3);
  assert.equal(result.events[1].length, 'binary:'.length + 70000 * 2);
  assert.equal(result.events[2], 'close:1000::true');

  result = await run({ endAfter: null, echoClose: true, onOpen: ws => ws.close() });
  assert.deepEqual(result.events, ['open', 'close:1005::true']);
  assert.deepEqual(result.fromClient, [{ opcode: 8, masked: true, payload: '' }]);

  result = await run({ endAfter: null, echoClose: true, onOpen: ws => ws.close(4000, 'done') });
  assert.deepEqual(result.events, ['open', 'close:4000:done:true']);
  assert.deepEqual(result.fromClient, [{ opcode: 8, masked: true, payload: '0fa0646f6e65' }]);

  result = await run({ endAfter: null, onOpen: ws => ws.close(1000) });
  assert.deepEqual(result.events, ['open', ...unclean]);

  console.log('websocket:client-protocol:ok');
})().catch(error => {
  console.error(error);
  process.exit(1);
});
