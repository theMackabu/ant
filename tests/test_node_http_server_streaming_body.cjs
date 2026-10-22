const assert = require('node:assert/strict');
const http = require('node:http');
const net = require('node:net');

function listen(server) {
  return new Promise(resolve => server.listen(0, '127.0.0.1', () => resolve(server.address().port)));
}

function close(server) {
  return new Promise(resolve => server.close(resolve));
}

function connect(port) {
  return new Promise(resolve => {
    const socket = net.connect(port, '127.0.0.1', () => resolve(socket));
    socket.setEncoding('latin1');
  });
}

function waitFor(socket, predicate) {
  return new Promise((resolve, reject) => {
    let text = '';
    const onData = chunk => {
      text += chunk;
      if (!predicate(text)) return;
      socket.off('data', onData);
      resolve(text);
    };
    socket.on('data', onData);
    socket.once('error', reject);
  });
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on('data', chunk => chunks.push(chunk));
    req.on('end', () => resolve(Buffer.concat(chunks).toString('latin1')));
    req.on('error', reject);
  });
}

async function requestBeforeBody() {
  let sawRequest;
  const requested = new Promise(resolve => { sawRequest = resolve; });
  const server = http.createServer(async (req, res) => {
    sawRequest();
    res.end(`got:${await readBody(req)}`);
  });
  const port = await listen(server);
  const socket = await connect(port);

  socket.write('POST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 10\r\n\r\nhello');
  await requested;
  const response = waitFor(socket, text => text.includes('got:'));
  socket.write('world');
  assert.match(await response, /got:helloworld$/);
  socket.destroy();
  await close(server);
}

async function chunkedPipelined() {
  const seen = [];
  const server = http.createServer(async (req, res) => {
    const body = await readBody(req);
    seen.push(`${req.url}=${body}`);
    res.end(body.toUpperCase());
  });
  const port = await listen(server);
  const socket = await connect(port);
  const response = waitFor(socket, text => text.includes('ABCDEF') && text.includes('LAST'));

  socket.write('POST /a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n');
  socket.write('3\r\ndef\r\n0\r\n\r\nPOST /b HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nlast');
  const text = await response;
  assert.ok(text.indexOf('ABCDEF') < text.indexOf('LAST'), text);
  assert.deepEqual(seen, ['/a=abcdef', '/b=last']);
  socket.destroy();
  await close(server);
}

async function expectContinue() {
  const server = http.createServer(async (req, res) => {
    res.end(`got:${await readBody(req)}`);
  });
  const port = await listen(server);
  const socket = await connect(port);

  const interim = waitFor(socket, text => text.includes('\r\n\r\n'));
  socket.write('POST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n');
  assert.equal(await interim, 'HTTP/1.1 100 Continue\r\n\r\n');
  const response = waitFor(socket, text => text.includes('got:'));
  socket.write('ok');
  assert.match(await response, /^HTTP\/1\.1 200 OK\r\n[\s\S]*got:ok$/);
  socket.destroy();

  let checked = 0;
  server.removeAllListeners('request');
  server.on('checkContinue', (req, res) => {
    checked++;
    res.writeHead(413, { connection: 'close' });
    res.end();
  });
  const rejected = await connect(port);
  const final = waitFor(rejected, text => text.includes('\r\n\r\n'));
  rejected.write('POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n');
  assert.match(await final, /^HTTP\/1\.1 413 /);
  assert.equal(checked, 1);
  rejected.destroy();
  await close(server);
}

async function abortMidBody() {
  let result;
  const done = new Promise(resolve => { result = resolve; });
  const server = http.createServer(async (req, res) => {
    let aborted = false;
    req.on('aborted', () => { aborted = true; });
    try {
      for await (const chunk of req) {
        assert.equal(chunk.toString(), 'part');
        client.destroy();
      }
      result({ ended: true });
    } catch (error) {
      result({ code: error.code, aborted, complete: req.complete });
    }
    res.end();
  });
  const port = await listen(server);
  const client = await connect(port);
  client.write('POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\npart');
  assert.deepEqual(await done, { code: 'ECONNRESET', aborted: true, complete: false });
  await close(server);
}

async function earlyResponseKeepAlive() {
  const urls = [];
  const server = http.createServer((req, res) => {
    urls.push(req.url);
    res.end(req.url);
  });
  const port = await listen(server);
  const socket = await connect(port);
  const response = waitFor(socket, text => text.endsWith('/second'));

  socket.write('POST /first HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\nab');
  await new Promise(resolve => setTimeout(resolve, 20));
  socket.write('cdefGET /second HTTP/1.1\r\nHost: x\r\n\r\n');
  await response;
  assert.deepEqual(urls, ['/first', '/second']);
  socket.destroy();
  await close(server);
}

async function largeBodySlowReader() {
  const size = 8 * 1024 * 1024;
  const server = http.createServer(async (req, res) => {
    let total = 0;
    for await (const chunk of req) {
      total += chunk.length;
      if (total % (1024 * 1024) < chunk.length) await new Promise(resolve => setImmediate(resolve));
    }
    res.end(String(total));
  });
  const port = await listen(server);
  const socket = await connect(port);
  const response = waitFor(socket, text => text.endsWith(String(size)));
  socket.write(`POST / HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: ${size}\r\n\r\n`);
  socket.write(Buffer.alloc(size, 0x61));
  await response;
  socket.destroy();
  await close(server);
}

(async () => {
  await requestBeforeBody();
  await chunkedPipelined();
  await expectContinue();
  await abortMidBody();
  await earlyResponseKeepAlive();
  await largeBodySlowReader();
  console.log('node:http server streaming body: ok');
})();
