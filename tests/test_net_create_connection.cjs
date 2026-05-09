const net = require('node:net');

let connected = false;
let serverSawData = false;
let clientSawData = false;
const timeout = setTimeout(() => {
  if (!connected || !serverSawData || !clientSawData) {
    throw new Error('net.createConnection timed out');
  }
}, 2000);

const server = net.createServer((socket) => {
  socket.setEncoding('utf8');
  socket.on('data', (chunk) => {
    if (chunk !== 'ping') throw new Error(`server received ${chunk}`);
    serverSawData = true;
    socket.write('pong');
  });
});

server.on('error', (err) => {
  throw err;
});

server.listen(0, '127.0.0.1', () => {
  const address = server.address();
  const client = net.createConnection({ port: address.port, host: '127.0.0.1' }, () => {
    connected = true;
    client.write('ping');
  });

  client.setEncoding('utf8');
  client.on('error', (err) => {
    throw err;
  });
  client.on('data', (chunk) => {
    if (chunk !== 'pong') throw new Error(`client received ${chunk}`);
    clientSawData = true;
    client.end();
    server.close(() => {
      if (!connected || !serverSawData || !clientSawData) {
        throw new Error('net.createConnection did not complete the loopback exchange');
      }
      clearTimeout(timeout);
      console.log('net:create-connection:ok');
    });
  });
});
