import fc from 'fast-check';
import { finish, literal } from '../prelude.mjs';
import { text } from './family.mjs';
import { http1Seeds, websocketSeeds } from './seeds.mjs';

export function splitterSizes(seed, length) {
  let state = (0x9e3779b9 ^ seed) >>> 0;
  const max = seed ? 1 + (seed % 32) : 1;
  const sizes = [];
  for (let remaining = length; remaining > 0;) {
    state = (state ^ (state << 13)) >>> 0;
    state = (state ^ (state >>> 17)) >>> 0;
    state = (state ^ (state << 5)) >>> 0;
    const size = Math.min(1 + (state % max), remaining);
    sizes.push(size);
    remaining -= size;
  }
  return sizes;
}

const httpMethods = ['GET', 'GET', 'POST', 'POST', 'PUT', 'HEAD', 'OPTIONS', 'DELETE', 'PATCH', 'get', 'G ET', 'PRI', 'CONNECT', 'M-SEARCH', ''];
const httpTargets = ['/', '/', '/a?b=c', '/a/../b', '*', 'http://h/x', '/%00', '/ä', '', '/ ', '/a#f', '//x', 'h:80'];
const httpVersions = ['HTTP/1.1', 'HTTP/1.1', 'HTTP/1.1', 'HTTP/1.0', 'HTTP/2.0', 'HTTP/1.1 ', 'http/1.1', 'HTTP/1', 'HTTP/1.10', ''];
const httpHeaders = [
  'Host: h', 'Host: h', 'Host: a', 'Host:', 'Content-Length: 3', 'Content-Length: 0', 'Content-Length: 03', 'Content-Length: -1',
  'Content-Length: 3, 3', 'Content-Length: 3\r\nContent-Length: 4', 'Content-Length: 1e1', 'Transfer-Encoding: chunked',
  'Transfer-Encoding: chunked', 'Transfer-Encoding: gzip, chunked', 'Transfer-Encoding: chunked, gzip', 'Transfer-Encoding : chunked',
  'Transfer-Encoding: Chunked', 'Transfer-Encoding: identity', 'X-A: b', 'X-A:b', 'X-A:  b  ', 'X-A: b\r\n c', 'X A: b',
  'X-A: \u0000', 'X-A: é', 'Connection: close', 'Connection: keep-alive', 'Expect: 100-continue', ':a: b', 'X-A',
  'Cookie: a=1; b=2', 'x-a: 1\r\nX-A: 2', 'Content-Type: text/plain', 'Host: 127.0.0.1:443', 'X-Empty:'
];
const chunkSizes = ['correct', 'correct', 'correct', 'short', 'long', 'extension', 'hex-case', 'garbage', 'negative'];

function chunkLine(kind, length) {
  if (kind === 'short') return Math.max(0, length - 1).toString(16);
  if (kind === 'long') return (length + 1).toString(16);
  if (kind === 'extension') return length.toString(16) + ';a=b';
  if (kind === 'hex-case') return length.toString(16).toUpperCase() + ' ';
  if (kind === 'garbage') return 'z';
  if (kind === 'negative') return '-' + length.toString(16);
  return length.toString(16);
}

const httpBody = fc.oneof(
  fc.constant({ kind: 'none' }),
  fc.record({ kind: fc.constant('fixed'), data: fc.string({ unit: fc.constantFrom('a', 'b', '\r', '\n', '0', ' '), maxLength: 6 }) }),
  fc.record({
    kind: fc.constant('chunked'),
    chunks: fc.array(fc.record({ size: fc.constantFrom(...chunkSizes), data: fc.string({ unit: fc.constantFrom('a', 'b', '\r', '\n', '0'), minLength: 1, maxLength: 5 }) }), { maxLength: 3 }),
    last: fc.constantFrom('0', '0', '0;x=y', '00', ''),
    trailer: fc.constantFrom('', '', 'X-T: 1', 'Content-Length: 9', 'X T: 1')
  })
);

const httpMessage = fc.record({
  method: fc.constantFrom(...httpMethods),
  target: fc.constantFrom(...httpTargets),
  version: fc.constantFrom(...httpVersions),
  headers: fc.array(fc.constantFrom(...httpHeaders), { maxLength: 4 }),
  eol: fc.constantFrom('\r\n', '\r\n', '\r\n', '\n'),
  body: httpBody
});

function encodeMessage({ method, target, version, headers, eol, body }) {
  let out = `${method} ${target} ${version}${eol}`;
  for (const header of headers) out += header + eol;
  out += eol;
  if (body.kind === 'fixed') out += body.data;
  if (body.kind === 'chunked') {
    for (const { size, data } of body.chunks) out += chunkLine(size, data.length) + eol + data + eol;
    out += body.last + eol + (body.trailer ? body.trailer + eol : '') + eol;
  }
  return out;
}

const httpProbe = String.raw`
const __net = require('node:net');
const __http = require('node:http');
async function __send(port, request, splits) {
  const socket = __net.connect(port, '127.0.0.1');
  socket.setNoDelay(true);
  const response = [];
  socket.on('data', data => response.push(...data));
  socket.on('error', () => {});
  await new Promise(resolve => { socket.on('connect', resolve); socket.on('error', resolve); });
  const bytes = Buffer.from(request, 'latin1');
  let offset = 0;
  for (const size of splits.concat([bytes.length])) {
    if (offset >= bytes.length || socket.destroyed) break;
    socket.write(bytes.subarray(offset, offset + size));
    offset += size;
    await new Promise(resolve => setTimeout(resolve, 2));
  }
  socket.end();
  await new Promise(resolve => { socket.on('close', resolve); setTimeout(resolve, 1000); });
  return Buffer.from(response).toString('latin1').match(/HTTP\/1\.[01] \d{3}/g);
}
async function __httpCase(label, request, splits) {
  const seen = [];
  const server = __http.createServer((req, res) => {
    const body = [];
    req.on('data', chunk => body.push(...chunk));
    req.on('end', () => {
      seen.push([req.method, req.url, req.httpVersion, req.rawHeaders, Buffer.from(body).toString('latin1'), req.rawTrailers]);
      res.end('ok');
    });
  });
  server.on('clientError', (error, socket) => {
    seen.push(['$clientError', error && error.code]);
    if (socket.writable) socket.end('HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n');
  });
  server.on('upgrade', (req, socket) => { seen.push(['$upgrade', req.method, req.url]); socket.destroy(); });
  server.on('connect', (req, socket) => { seen.push(['$connect', req.method, req.url]); socket.destroy(); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const statuses = await __send(server.address().port, request, splits);
  server.closeAllConnections();
  server.close();
  __out.push(['requests:' + label, __canon(seen)]);
  __out.push(['statuses:' + label, __canon(statuses)]);
  if (typeof Ant === 'undefined' || typeof Ant.serve !== 'function') { __out.push(['serve-split:' + label, true]); return; }
  const serve = async sizes => {
    const requests = [];
    const app = Ant.serve({
      hostname: '127.0.0.1',
      port: 0,
      async fetch(req) {
        try {
          const url = new URL(req.url);
          requests.push([req.method, url.pathname + url.search, Array.from(req.headers), Array.from(new Uint8Array(await req.arrayBuffer()))]);
        } catch (error) {
          requests.push(['$error', error && error.name]);
        }
        return new Response('ok');
      }
    });
    const result = await __send(app.port, request, sizes);
    await app.stop(true);
    return [requests, result];
  };
  const whole = await serve([]);
  const split = await serve(splits);
  __out.push(['serve-split:' + label, JSON.stringify(whole) === JSON.stringify(split) || __canon([whole, split])]);
}`;

export const http1 = {
  name: 'http1',
  arbitrary: fc.record({
    connections: fc.array(
      fc.record({
        messages: fc.array(httpMessage, { minLength: 1, maxLength: 3 }),
        splits: fc.array(fc.integer({ min: 1, max: 24 }), { maxLength: 8 })
      }),
      { minLength: 1, maxLength: 3 }
    )
  }),
  build({ connections }) {
    const body = connections.map(({ messages, splits }, i) => `await __httpCase(${literal(String(i))}, ${literal(messages.map(encodeMessage).join(''))}, ${literal(splits)});`);
    for (const seed of http1Seeds) body.push(`await __httpCase(${literal(`seed:${seed.name}`)}, ${literal(seed.request)}, ${literal(splitterSizes(seed.seed, seed.request.length))});`);
    return finish(`${httpProbe}\n${body.join('\n')}`, true);
  }
};

const utf8 = value => Array.from(new TextEncoder().encode(value));
const closeCodes = [1000, 1001, 1002, 1005, 1006, 1007, 1011, 1015, 999, 1004, 1016, 2999, 3000, 4999, 5000];

const wsPayload = fc.oneof(
  { weight: 3, arbitrary: text(6).map(value => ({ kind: 'text', bytes: utf8(value) })) },
  { weight: 2, arbitrary: fc.array(fc.integer({ min: 0, max: 255 }), { maxLength: 8 }).map(bytes => ({ kind: 'bytes', bytes })) },
  { weight: 1, arbitrary: fc.constantFrom([0xff], [0xc3], [0xed, 0xa0, 0x80], [0xf4, 0x90, 0x80, 0x80], [0xe2, 0x82]).map(bytes => ({ kind: 'invalid', bytes })) },
  { weight: 1, arbitrary: fc.integer({ min: 126, max: 300 }).map(length => ({ kind: 'long', bytes: Array.from({ length }, (_, i) => 97 + (i % 26)) })) },
  { weight: 2, arbitrary: fc.tuple(fc.constantFrom(...closeCodes), fc.constantFrom('', 'bye', 'é', 'x'.repeat(130))).map(([code, reason]) => ({ kind: 'close', bytes: [code >> 8, code & 255, ...utf8(reason)] })) },
  { weight: 1, arbitrary: fc.constantFrom({ kind: 'close', bytes: [] }, { kind: 'close', bytes: [3] }) }
);

const wsFrame = fc.record({
  fin: fc.constantFrom(true, true, true, false),
  rsv: fc.constantFrom(0, 0, 0, 0, 0, 4, 2, 1),
  opcode: fc.constantFrom(1, 1, 2, 2, 0, 9, 10, 8, 3, 11),
  mask: fc.constantFrom(false, false, false, false, true),
  lengthForm: fc.constantFrom('min', 'min', 'min', '126', '127'),
  payload: wsPayload
});

function encodeFrame({ fin, rsv, opcode, mask, lengthForm, payload, key = [0x12, 0x34, 0x56, 0x78] }) {
  const bytes = [(fin ? 0x80 : 0) | (rsv << 4) | opcode];
  const length = payload.bytes.length;
  const maskBit = mask ? 0x80 : 0;
  if (lengthForm === '127' || length > 65535) bytes.push(maskBit | 127, 0, 0, 0, 0, (length >>> 24) & 255, (length >>> 16) & 255, (length >>> 8) & 255, length & 255);
  else if (lengthForm === '126' || length > 125) bytes.push(maskBit | 126, (length >> 8) & 255, length & 255);
  else bytes.push(maskBit | length);
  if (mask) bytes.push(...key);
  payload.bytes.forEach((byte, i) => bytes.push(mask ? byte ^ key[i & 3] : byte));
  return bytes;
}

const frameReader = String.raw`
function __payload(bytes) {
  if (bytes.length <= 256) return Array.from(bytes);
  let hash = 0;
  for (const byte of bytes) hash = (Math.imul(hash, 31) + byte) | 0;
  return ['$len', bytes.length, hash];
}
function __readFrames(buffer, out) {
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const b0 = buffer[offset];
    const b1 = buffer[offset + 1];
    let length = b1 & 127;
    let at = offset + 2;
    if (length === 126) { if (buffer.length < at + 2) break; length = buffer.readUInt16BE(at); at += 2; }
    else if (length === 127) { if (buffer.length < at + 8) break; length = Number(buffer.readBigUInt64BE(at)); at += 8; }
    const masked = (b1 & 128) !== 0;
    const key = masked ? buffer.subarray(at, at + 4) : null;
    if (masked) at += 4;
    if (buffer.length < at + length) break;
    const payload = Buffer.from(buffer.subarray(at, at + length));
    if (masked) for (let i = 0; i < payload.length; i++) payload[i] ^= key[i & 3];
    out.push([b0 >> 7, (b0 >> 4) & 7, b0 & 15, masked, __payload(payload)]);
    offset = at + length;
  }
  return buffer.subarray(offset);
}`;

const wsClientProbe = String.raw`
const __net = require('node:net');
const __crypto = require('node:crypto');
async function __wsCase(label, frameText, splits, sends) {
  const frames = Buffer.from(frameText, 'latin1');
  const events = [];
  const fromClient = [];
  const server = __net.createServer(socket => {
    let buffer = Buffer.alloc(0);
    let upgraded = false;
    socket.on('error', () => {});
    socket.on('data', async data => {
      buffer = Buffer.concat([buffer, data]);
      if (!upgraded) {
        const end = buffer.indexOf('\r\n\r\n');
        if (end < 0) return;
        const key = /sec-websocket-key: *(\S+)/i.exec(buffer.subarray(0, end).toString('latin1'))[1];
        buffer = buffer.subarray(end + 4);
        upgraded = true;
        const accept = __crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
        await new Promise(resolve => setTimeout(resolve, 50));
        let offset = 0;
        for (const size of splits.concat([frames.length])) {
          if (offset >= frames.length) break;
          await new Promise(resolve => setTimeout(resolve, 2));
          if (socket.destroyed) return;
          socket.write(frames.subarray(offset, offset + size));
          offset += size;
        }
        setTimeout(() => socket.end(), 300);
      }
      const before = fromClient.length;
      buffer = __readFrames(buffer, fromClient);
      if (fromClient.slice(before).some(frame => frame[2] === 8)) socket.end();
    });
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const ws = new WebSocket('ws://127.0.0.1:' + server.address().port);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    events.push(['open']);
    for (const [kind, value] of sends) ws.send(kind === 'text' ? value : new Uint8Array(value).map((_, i) => i * 7));
  };
  ws.onmessage = event => events.push(['message', typeof event.data === 'string' ? event.data : Array.from(new Uint8Array(event.data))]);
  ws.onerror = () => events.push(['error']);
  await new Promise(resolve => {
    ws.onclose = event => { events.push(['close', event.code, event.reason, event.wasClean]); resolve(); };
    setTimeout(resolve, 2000);
  });
  server.close();
  __out.push(['events:' + label, __canon(events)]);
  __out.push(['client-frames:' + label, __canon(fromClient)]);
  __out.push(['ready-state:' + label, ws.readyState]);
}`;

const wsSend = fc.oneof(
  text(6).map(value => ['text', value]),
  fc.constantFrom(0, 1, 125, 126, 127, 65535, 65536, 70000).map(length => ['binary', length]),
  fc.constantFrom(130, 65536).map(length => ['text', 'é'.repeat(length / 2)])
);

export const websocket = {
  name: 'websocket',
  arbitrary: fc.record({
    connections: fc.array(
      fc.record({
        frames: fc.array(wsFrame, { minLength: 1, maxLength: 4 }),
        splits: fc.array(fc.integer({ min: 1, max: 12 }), { maxLength: 6 }),
        sends: fc.array(wsSend, { maxLength: 3 })
      }),
      { minLength: 1, maxLength: 2 }
    )
  }),
  build({ connections }) {
    const body = connections.map(({ frames, splits, sends }, i) => {
      const bytes = frames.flatMap(encodeFrame);
      return `await __wsCase(${literal(String(i))}, ${literal(Buffer.from(bytes).toString('latin1'))}, ${literal(splits)}, ${literal(sends)});`;
    });
    for (const seed of websocketSeeds) body.push(`await __wsCase(${literal(`seed:${seed.name}`)}, ${literal(seed.frames)}, ${literal(splitterSizes(0, Math.min(seed.frames.length, 16)))}, []);`);
    return finish(`${frameReader}\n${wsClientProbe}\n${body.join('\n')}`, true);
  }
};

const maskKey = fc.tuple(fc.integer({ min: 0, max: 255 }), fc.integer({ min: 0, max: 255 }), fc.integer({ min: 0, max: 255 }), fc.integer({ min: 0, max: 255 }));
const serverMessage = fc.record({
  type: fc.constantFrom('text', 'text', 'binary'),
  text: fc.oneof(text(8), fc.constantFrom('é'.repeat(70), 'x'.repeat(200), '😀'.repeat(20000))),
  length: fc.constantFrom(0, 1, 3, 4, 5, 125, 126, 127, 300, 65535, 65536),
  pieces: fc.array(fc.double({ min: 0, max: 1, noNaN: true }), { maxLength: 2 }),
  pingBefore: fc.constantFrom(false, false, false, true),
  keys: fc.array(maskKey, { minLength: 3, maxLength: 3 }),
  lengthForm: fc.constantFrom('min', 'min', 'min', '126', '127')
});
const serverMutations = ['none', 'none', 'none', 'unmasked', 'rsv', 'reserved-opcode', 'control-fragmented', 'control-long', 'bad-utf8', 'orphan-continuation', 'interleaved-data', 'oversize'];

function serverBytes(message, i) {
  if (message.type === 'binary') return Array.from({ length: message.length }, (_, k) => (k * 31 + i) & 255);
  return utf8(message.text);
}

function serverFrames(messages, closeCode, mutation, mutateAt, maxPayload) {
  const frames = [];
  const expected = [];
  messages.forEach((message, i) => {
    const bytes = serverBytes(message, i);
    const cuts = [...new Set(message.pieces.map(p => Math.floor(p * bytes.length)))].sort((a, b) => a - b).filter(c => c > 0 && c < bytes.length);
    const bounds = [0, ...cuts, bytes.length];
    if (message.pingBefore) frames.push({ fin: true, rsv: 0, opcode: 9, mask: true, lengthForm: 'min', payload: { bytes: [112, 105] }, key: message.keys[2] });
    for (let k = 0; k + 1 < bounds.length; k++) {
      frames.push({
        fin: k + 2 === bounds.length,
        rsv: 0,
        opcode: k === 0 ? (message.type === 'text' ? 1 : 2) : 0,
        mask: true,
        lengthForm: message.lengthForm,
        payload: { bytes: bytes.slice(bounds[k], bounds[k + 1]) },
        key: message.keys[k % 2]
      });
      if (k === 0 && bounds.length > 2 && message.pingBefore) frames.push({ fin: true, rsv: 0, opcode: 10, mask: true, lengthForm: 'min', payload: { bytes: [] }, key: message.keys[2] });
    }
    expected.push(message.type === 'text' ? ['text', message.text] : ['binary', bytes.length <= 256 ? bytes : ['$len', bytes.length, bytes.reduce((h, b) => (Math.imul(h, 31) + b) | 0, 0)]]);
  });
  if (closeCode) frames.push({ fin: true, rsv: 0, opcode: 8, mask: true, lengthForm: 'min', payload: { bytes: [closeCode >> 8, closeCode & 255] }, key: [1, 2, 3, 4] });
  const limited = maxPayload !== null && messages.some((message, i) => serverBytes(message, i).length > maxPayload);
  if (mutation === 'none' || frames.length === 0) return { frames, expected: limited ? null : expected };
  const target = frames[Math.floor(mutateAt * frames.length) % frames.length];
  if (mutation === 'unmasked') target.mask = false;
  if (mutation === 'rsv') target.rsv = 4;
  if (mutation === 'reserved-opcode') target.opcode = 3;
  if (mutation === 'control-fragmented') frames.splice(frames.indexOf(target), 0, { fin: false, rsv: 0, opcode: 9, mask: true, lengthForm: 'min', payload: { bytes: [1] }, key: [9, 9, 9, 9] });
  if (mutation === 'control-long') frames.splice(frames.indexOf(target), 0, { fin: true, rsv: 0, opcode: 9, mask: true, lengthForm: 'min', payload: { bytes: Array(126).fill(1) }, key: [9, 9, 9, 9] });
  if (mutation === 'bad-utf8') frames.splice(frames.indexOf(target), 0, { fin: true, rsv: 0, opcode: 1, mask: true, lengthForm: 'min', payload: { bytes: [0xc3, 0x28] }, key: [7, 7, 7, 7] });
  if (mutation === 'orphan-continuation') frames.splice(frames.indexOf(target), 0, { fin: true, rsv: 0, opcode: 0, mask: true, lengthForm: 'min', payload: { bytes: [65] }, key: [5, 5, 5, 5] });
  if (mutation === 'interleaved-data') frames.splice(frames.indexOf(target), 0, { fin: false, rsv: 0, opcode: 1, mask: true, lengthForm: 'min', payload: { bytes: [65] }, key: [5, 5, 5, 5] }, { fin: true, rsv: 0, opcode: 2, mask: true, lengthForm: 'min', payload: { bytes: [66] }, key: [5, 5, 5, 5] });
  if (mutation === 'oversize') frames.splice(frames.indexOf(target), 0, { fin: true, rsv: 0, opcode: 2, mask: true, lengthForm: 'min', payload: { bytes: Array((maxPayload ?? 65536) + 1).fill(2) }, key: [3, 3, 3, 3] });
  return { frames, expected: null };
}

const wsServerProbe = String.raw`
const __net = require('node:net');
async function __antServeRun(frames, sizes, maxPayload) {
  const events = [];
  const options = {
    hostname: '127.0.0.1',
    port: 0,
    fetch(req, ctx) {
      const { socket, response } = ctx.upgradeWebSocket(req);
      socket.binaryType = 'arraybuffer';
      socket.onmessage = event => {
        const data = typeof event.data === 'string' ? ['text', event.data] : ['binary', __payload(new Uint8Array(event.data))];
        events.push(['message', data[0], data[1], typeof event.data === 'string' ? event.data.length : event.data.byteLength]);
        socket.send(event.data);
      };
      socket.onclose = event => events.push(['close', event.code]);
      socket.onerror = () => events.push(['error']);
      return response;
    }
  };
  if (maxPayload !== null) options.websocket = { maxPayloadLength: maxPayload };
  const app = Ant.serve(options);
  const socket = __net.connect(app.port, '127.0.0.1');
  socket.setNoDelay(true);
  socket.on('error', () => {});
  let buffer = Buffer.alloc(0);
  let upgraded = false;
  const fromServer = [];
  let closed = false;
  socket.on('data', data => {
    buffer = Buffer.concat([buffer, data]);
    if (!upgraded) {
      const end = buffer.indexOf('\r\n\r\n');
      if (end < 0) return;
      upgraded = buffer.subarray(0, 12).toString('latin1');
      buffer = buffer.subarray(end + 4);
    }
    buffer = __readFrames(buffer, fromServer);
  });
  const done = new Promise(resolve => { socket.on('close', () => { closed = true; resolve(); }); });
  await new Promise(resolve => socket.on('connect', resolve));
  socket.write('GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n');
  for (let i = 0; i < 100 && !upgraded; i++) await new Promise(resolve => setTimeout(resolve, 5));
  let offset = 0;
  for (const size of sizes.concat([frames.length])) {
    if (offset >= frames.length || socket.destroyed) break;
    socket.write(frames.subarray(offset, offset + size));
    offset += size;
    await new Promise(resolve => setTimeout(resolve, 2));
  }
  await Promise.race([done, new Promise(resolve => setTimeout(resolve, 500))]);
  if (!closed) socket.end();
  await Promise.race([done, new Promise(resolve => setTimeout(resolve, 500))]);
  await app.stop(true);
  return [upgraded, events, fromServer];
}
async function __wsServerCase(label, frameText, sizes, maxPayload, expected) {
  if (typeof Ant === 'undefined' || typeof Ant.serve !== 'function') {
    __out.push(['messages:' + label, __canon(expected === null ? '$unchecked' : expected)]);
    __out.push(['echoes:' + label, __canon(expected === null ? '$unchecked' : expected.map(([type, data]) => [type === 'text' ? 1 : 2, type]))]);
    __out.push(['split:' + label, true]);
    __out.push(['limit:' + label, false]);
    __out.push(['echo-payload:' + label, true]);
    return;
  }
  const frames = Buffer.from(frameText, 'latin1');
  const whole = await __antServeRun(frames, [], maxPayload);
  const split = await __antServeRun(frames, sizes, maxPayload);
  const messages = whole[1].filter(event => event[0] === 'message');
  __out.push(['messages:' + label, __canon(expected === null ? '$unchecked' : messages.map(event => [event[1], event[2]]))]);
  __out.push(['echoes:' + label, __canon(expected === null ? '$unchecked' : whole[2].filter(frame => frame[2] === 1 || frame[2] === 2).map(frame => [frame[2], frame[2] === 1 ? 'text' : 'binary']))]);
  __out.push(['split:' + label, JSON.stringify(whole) === JSON.stringify(split) || __canon([whole, split])]);
  __out.push(['limit:' + label, maxPayload !== null && messages.some(event => event[3] > maxPayload)]);
  const echoed = whole[2].filter(frame => frame[2] === 1 || frame[2] === 2).map(frame => [frame[3], frame[4]]);
  const sent = messages.map(event => [false, event[1] === 'text' ? __payload(Buffer.from(event[2])) : event[2]]);
  __out.push(['echo-payload:' + label, JSON.stringify(echoed) === JSON.stringify(sent) || __canon([echoed, sent])]);
}`;

export const websocketServer = {
  name: 'websocket-server',
  arbitrary: fc.record({
    connections: fc.array(
      fc.record({
        messages: fc.array(serverMessage, { minLength: 1, maxLength: 3 }),
        closeCode: fc.constantFrom(0, 1000, 1000, 1001, 3000),
        mutation: fc.constantFrom(...serverMutations),
        mutateAt: fc.double({ min: 0, max: 1, noNaN: true }),
        maxPayload: fc.constantFrom(null, null, null, 64, 200),
        splits: fc.array(fc.integer({ min: 1, max: 12 }), { maxLength: 8 })
      }),
      { minLength: 1, maxLength: 2 }
    )
  }),
  build({ connections }) {
    const body = connections.map(({ messages, closeCode, mutation, mutateAt, maxPayload, splits }, i) => {
      const { frames, expected } = serverFrames(messages, closeCode, mutation, mutateAt, maxPayload);
      return `await __wsServerCase(${literal(String(i))}, ${literal(Buffer.from(frames.flatMap(encodeFrame)).toString('latin1'))}, ${literal(splits)}, ${literal(maxPayload)}, ${literal(expected)});`;
    });
    for (const seed of websocketSeeds) body.push(`await __wsServerCase(${literal(`seed:${seed.name}`)}, ${literal(seed.frames)}, ${literal(splitterSizes(0, Math.min(seed.frames.length, 16)))}, ${literal(seed.maxPayload)}, null);`);
    return finish(`${frameReader}\n${wsServerProbe}\n${body.join('\n')}`, true);
  }
};
