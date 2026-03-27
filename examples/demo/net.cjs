const net = require('node:net');

const httpResponse = `HTTP/1.1 200 OK\r
Content-Type: text/plain\r
\r
Hello, World from net module!
`;

const server = net.createServer(socket => {
  console.log('Client connected');

  socket.on('data', () => {
    socket.write(httpResponse);
    socket.end();
  });

  socket.on('end', () => {
    console.log('Client disconnected');
  });

  socket.on('error', err => {
    console.error(`Socket error: ${err.message}`);
  });
});

server.listen(3000, () => console.log(`server on http://localhost:3000`));
