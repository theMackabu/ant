const decoder = new TextDecoder();
const port = Number(process.argv[2] || 32187);

export default {
  hostname: '127.0.0.1',
  port,
  fetch(request, ctx) {
    const url = new URL(request.url);
    if (url.pathname !== '/ws') return new Response('not found', { status: 404 });

    const { socket, response } = ctx.upgradeWebSocket(request);
    socket.onmessage = event => {
      const data = typeof event.data === 'string' ? event.data : decoder.decode(event.data);
      socket.send(typeof event.data + ':' + data);
      if (data === 'close') {
        socket.close(1000, 'done');
        setTimeout(() => ctx.stop(), 20);
      }
    };
    return response;
  }
};
