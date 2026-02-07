let count = 0;
const MAX_REQUESTS = 10;

console.log('starting on http://localhost:3000');

Ant.serve(3000, (ctx, server) => {
  if (ctx.req.uri.includes('favicon')) return;

  count++;
  ctx.res.json({
    request: count,
    remaining: Math.max(0, MAX_REQUESTS - count),
    port: server.port
  });

  if (count === MAX_REQUESTS) {
    console.log('10 requests served, stopping');
    server.stop();
  }
});
