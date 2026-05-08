const port = Number(process.argv[2] || 32188);
let retryCount = 0;

export default {
  hostname: '127.0.0.1',
  port,
  fetch(request, ctx) {
    const url = new URL(request.url);

    if (url.pathname === '/events') {
      const stream = ctx.eventSource();
      setTimeout(() => {
        stream.comment('ready');
        stream.send({
          event: 'greeting',
          id: '42',
          retry: 50,
          data: 'hello\nworld'
        });
        stream.close();
      }, 20);
      return stream.response;
    }

    if (url.pathname === '/retry') {
      retryCount++;
      const stream = ctx.eventSource();
      setTimeout(() => {
        stream.send({
          event: 'tick',
          retry: 50,
          data: retryCount === 1 ? 'first' : 'second'
        });
        stream.close();
        if (retryCount >= 2) setTimeout(() => ctx.stop(), 20);
      }, 20);
      return stream.response;
    }

    return new Response('not found', { status: 404 });
  }
};
