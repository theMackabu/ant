const assert = require('node:assert');
const http = require('node:http');
const net = require('node:net');

const watchdog = setTimeout(() => {
  console.error('timed out');
  process.exit(1);
}, 8000);

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const traces = new Map();
const closed = new Map();

function trace(request, response, log) {
  request.on('aborted', () => log.push('req aborted'));
  request.on('end', () => log.push('req end'));
  request.on('close', () => log.push('req close'));
  request.on('error', error => log.push(`req error ${error.code}`));
  response.on('close', () => log.push('res close'));
  response.on('finish', () => log.push('res finish'));
}

const server = http.createServer((request, response) => {
  const log = [];
  traces.set(request.url, { log, request, response });
  trace(request, response, log);
  closed.get(request.url)?.(request, response);

  if (request.url === '/finished') return response.end('done');
  if (request.url === '/late-listeners') {
    response.end('done', () => log.push('end callback'));
    log.push(`after end finished=${response.writableFinished} closed=${response.closed}`);
    response.on('finish', () => log.push('late finish'));
    response.on('close', () => log.push(`late close closed=${response.closed}`));
    return;
  }
  if (request.url === '/read') {
    request.on('data', () => {});
    request.on('end', () => response.end('done'));
    return;
  }
  if (request.url === '/unread-partial') return response.end('done');
  if (request.url === '/destroy-then-end') {
    response.write('part');
    response.destroy();
    return response.end();
  }
  if (request.url === '/destroy') {
    response.write('part');
    return setTimeout(() => response.destroy(), 5);
  }
  if (request.method === 'POST' && request.url === '/body') {
    request.on('data', () => {});
    request.on('end', () => response.write('part'));
    return;
  }
  if (request.method === 'POST') {
    request.on('data', () => {});
    return;
  }
  response.writeHead(200);
  response.write('part');
});

function client(path, { method = 'GET', headers = '', body = '', after } = {}) {
  return new Promise(resolve => {
    const socket = net.connect(server.address().port, '127.0.0.1', () => {
      socket.write(`${method} ${path} HTTP/1.1\r\nHost: x\r\n${headers}\r\n${body}`);
      if (after) after(socket);
    });
    socket.on('error', () => {});
    socket.on('close', resolve);
    socket.once('data', () => {
      if (!after) socket.destroy();
    });
  });
}

const logOf = path => traces.get(path).log;

async function waitFor(path, event) {
  for (let i = 0; i < 200 && !logOf(path).includes(event); i++) await sleep(5);
  await sleep(10);
}

async function main() {
  await client('/get');
  await waitFor('/get', 'req close');
  assert.deepStrictEqual(logOf('/get').filter(e => !e.startsWith('req error')), ['req aborted', 'res close', 'req close']);
  assert.strictEqual(traces.get('/get').request.aborted, true);
  assert.strictEqual(traces.get('/get').response.writableFinished, false);
  assert.strictEqual(traces.get('/get').response.destroyed, true);

  await client('/body', { method: 'POST', headers: 'Content-Length: 2\r\n', body: 'ok' });
  await waitFor('/body', 'req close');
  assert.ok(logOf('/body').includes('res close'));
  assert.ok(logOf('/body').includes('req close'));

  await client('/partial', {
    method: 'POST',
    headers: 'Content-Length: 100\r\n',
    body: 'short',
    after: socket => setTimeout(() => socket.destroy(), 10),
  });
  await waitFor('/partial', 'req close');
  assert.ok(logOf('/partial').includes('req aborted'));
  assert.ok(logOf('/partial').includes('res close'));

  await client('/finished', { headers: 'Connection: close\r\n', after: () => {} });
  await waitFor('/finished', 'req close');
  assert.deepStrictEqual(logOf('/finished').filter(e => e.startsWith('res')), ['res finish', 'res close']);
  assert.ok(!logOf('/finished').includes('req aborted'));

  await client('/late-listeners', { headers: 'Connection: close\r\n', after: () => {} });
  await waitFor('/late-listeners', 'req close');
  assert.deepStrictEqual(logOf('/late-listeners'), [
    'after end finished=false closed=false',
    'end callback',
    'res finish',
    'late finish',
    'req end',
    'res close',
    'late close closed=true',
    'req close',
  ]);

  await client('/read', { method: 'POST', headers: 'Connection: close\r\nContent-Length: 2\r\n', body: 'ok', after: () => {} });
  await waitFor('/read', 'res close');
  assert.deepStrictEqual(logOf('/read'), ['req end', 'res finish', 'req close', 'res close']);

  await client('/unread-partial', {
    method: 'POST',
    headers: 'Connection: close\r\nContent-Length: 50\r\n',
    body: 'ok',
    after: () => {},
  });
  await waitFor('/unread-partial', 'res close');
  await sleep(20);
  assert.deepStrictEqual(logOf('/unread-partial'), ['res finish', 'res close']);

  await client('/destroy', { after: () => {} });
  await waitFor('/destroy', 'res close');
  assert.ok(logOf('/destroy').includes('res close'));

  await client('/destroy-then-end', { after: () => {} });
  await waitFor('/destroy-then-end', 'res close');
  assert.strictEqual(logOf('/destroy-then-end').filter(e => e === 'res close').length, 1);
  assert.ok(!logOf('/destroy-then-end').includes('res finish'));

  const { response } = traces.get('/get');
  const results = [];
  const returned = response.write('late', error => results.push(error.code));
  response.end('late', () => results.push('end callback'));
  await sleep(10);
  assert.strictEqual(returned, false);
  assert.deepStrictEqual(results, ['ERR_STREAM_DESTROYED']);
  assert.strictEqual(response.writableEnded, true);
  assert.strictEqual(logOf('/get').filter(e => e === 'res close').length, 1);
  assert.ok(!logOf('/get').includes('res finish'));

  server.close();
  clearTimeout(watchdog);
  console.log('http-server-disconnect-events:ok');
}

server.listen(0, '127.0.0.1', () => {
  main().catch(error => {
    console.error(error && error.stack ? error.stack : String(error));
    process.exit(1);
  });
});
