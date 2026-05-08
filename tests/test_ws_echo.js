const socket = new WebSocket('wss://echo.websocket.org');

socket.addEventListener('open', _ => {
  console.log('Connected to echo server');
  socket.send('Hello, WebSocket Echo Server!');
  socket.send(JSON.stringify({ type: 'test', timestamp: Date.now(), message: 'Testing echo functionality' }));
});

socket.addEventListener('message', event => {
  console.log('Echoed back:', event.data);

  try {
    const data = JSON.parse(event.data);
    console.log('Received JSON:', data);
    process.exit(0);
  } catch (e) {
    console.log('Received text:', event.data);
  }
});

socket.addEventListener('error', event => {
  console.error('WebSocket error:', event);
  process.exit(0);
});

socket.addEventListener('close', event => {
  console.log('Disconnected from echo server');
  console.log('Close code:', event.code);
  console.log('Close reason:', event.reason);
  process.exit(0);
});
