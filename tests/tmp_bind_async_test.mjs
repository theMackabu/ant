import net from 'node:net';

// replicate exactly what vite does
const server = net.createServer();
const origListen = server.listen.bind(server);

console.log('server type:', typeof server);
console.log('origListen type:', typeof origListen);

server.listen = async (port, ...args) => {
  console.log('async wrapper called, port=', port, 'args=', args);
  try {
    await Promise.resolve(); // simulate initServer
    console.log('after await, calling origListen...');
    return origListen(port, ...args);
  } catch (e) {
    server.emit('error', e);
    return;
  }
};

await new Promise((resolve, reject) => {
  server.on('error', (e) => {
    console.log('error event:', e.message);
    reject(e);
  });
  server.on('listening', () => {
    console.log('listening on', server.address());
    server.close(resolve);
  });
  server.listen(0, '127.0.0.1');
});

console.log('done');
