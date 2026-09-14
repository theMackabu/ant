// on<type> handlers register a listener on first assignment (HTML event
// handler rules), and ws.on/once/off wrap the listener table.
const assert = require('node:assert');
const events = require('node:events');

const server = Ant.serve({
  hostname: '127.0.0.1',
  port: 0,
  fetch(request, ctx) {
    const url = new URL(request.url);
    const { socket, response } = ctx.upgradeWebSocket(request);
    const order = [];
    const report = () => socket.send(JSON.stringify(order));

    if (url.pathname === '/handler-first') {
      socket.onmessage = () => order.push('handler');
      socket.addEventListener('message', () => { order.push('listener'); report(); });
    } else if (url.pathname === '/listener-first') {
      socket.addEventListener('message', () => order.push('listener'));
      socket.onmessage = () => { order.push('handler'); report(); };
    } else if (url.pathname === '/reassign') {
      // Reassigning keeps the original position; the old callback never runs.
      socket.onmessage = () => order.push('old');
      socket.addEventListener('message', () => order.push('listener'));
      socket.onmessage = () => order.push('new');
      socket.addEventListener('message', report);
    } else if (url.pathname === '/null-then-set') {
      // Assigning null removes the listener; setting again appends at the end.
      socket.onmessage = () => order.push('first');
      socket.addEventListener('message', () => order.push('listener'));
      socket.onmessage = null;
      socket.onmessage = () => order.push('second');
      socket.addEventListener('message', report);
    } else if (url.pathname === '/getter') {
      const fn = () => {};
      const before = socket.onmessage;
      socket.onmessage = fn;
      const same = socket.onmessage === fn;
      socket.onmessage = null;
      const after = socket.onmessage;
      socket.onmessage = () => socket.send(JSON.stringify({ before, same, after }));
    } else if (url.pathname === '/on-api') {
      const chained = socket.on('message', () => order.push('on')) === socket;
      const off = () => order.push('off');
      socket.on('message', off).off('message', off);
      socket.once('message', () => order.push('once'));
      socket.on('message', () => { order.push(chained ? 'chained' : 'not-chained'); report(); order.length = 0; });
    }
    return response;
  }
});

function open(path) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(`ws://127.0.0.1:${server.port}${path}`);
    ws.onopen = () => resolve(ws);
    ws.onerror = () => reject(new Error('open failed'));
  });
}

function next(ws) {
  return new Promise(resolve => { ws.onmessage = e => resolve(JSON.parse(e.data)); });
}

const sockets = [];

async function roundTrip(path, payload = 'x') {
  const ws = await open(path);
  sockets.push(ws);
  const reply = next(ws);
  ws.send(payload);
  const result = await reply;
  return { ws, result };
}

function closed(ws) {
  return new Promise(resolve => {
    if (ws.readyState === WebSocket.CLOSED) return resolve();
    ws.addEventListener('close', () => resolve(), { once: true });
    ws.close();
  });
}

async function main() {
  assert.deepEqual((await roundTrip('/handler-first')).result, ['handler', 'listener']);
  assert.deepEqual((await roundTrip('/listener-first')).result, ['listener', 'handler']);
  assert.deepEqual((await roundTrip('/reassign')).result, ['new', 'listener']);
  assert.deepEqual((await roundTrip('/null-then-set')).result, ['listener', 'second']);
  assert.deepEqual((await roundTrip('/getter')).result, { before: null, same: true, after: null });

  const { ws: onApi, result: first } = await roundTrip('/on-api');
  assert.deepEqual(first, ['on', 'once', 'chained']);
  const second = next(onApi);
  onApi.send('y');
  assert.deepEqual(await second, ['on', 'chained']);

  // Client side: the same accessor semantics, plus events.once() still works.
  const client = new WebSocket(`ws://127.0.0.1:${server.port}/handler-first`);
  assert.equal(client.onopen, null);
  assert.equal(typeof client.on, 'function');
  const [openEvent] = await events.once(client, 'open');
  assert.equal(openEvent.type, 'open');
  const order = [];
  client.onmessage = () => order.push('handler');
  client.on('message', () => order.push('on'));
  client.send('z');
  await new Promise(resolve => client.addEventListener('message', resolve, { once: true }));
  assert.deepEqual(order, ['handler', 'on']);
  sockets.push(client);

  // Graceful stop waits for connections to drain, so close every client first.
  await Promise.all(sockets.map(closed));
  await server.stop();
  console.log('websocket:handlers:ok');
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exit(1);
});
