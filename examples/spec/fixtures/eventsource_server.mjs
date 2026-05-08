const port = Number(process.argv[2] || 32188);

export default {
  hostname: '127.0.0.1',
  port,
  fetch(request, ctx) {
    const url = new URL(request.url);
    if (url.pathname !== '/events') return new Response('not found', { status: 404 });

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
      setTimeout(() => ctx.stop(), 20);
    }, 20);
    return stream.response;
  }
};
