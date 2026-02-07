let count = 0;

console.log('starting on http://localhost:3000');

Ant.serve(3000, (ctx, server) => {
  if (ctx.req.uri.includes('favicon')) return;

  count++;
  ctx.res.json({
    request: count,
    remaining: 10 - count,
    port: server.port
  });

  if (count >= 10) {
    console.log('10 requests served, stopping');
    server.stop();
  }
});
