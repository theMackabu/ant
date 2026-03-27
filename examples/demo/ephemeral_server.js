let count = 0;
const MAX_REQUESTS = 10;

console.log('starting on http://localhost:3000');

export default {
  fetch(req, server) {
    if (new URL(req.url).pathname.includes('favicon')) {
      return new Response(null, { status: 404 });
    }

    count++;

    if (count === MAX_REQUESTS) {
      console.log('10 requests served, stopping');
      server.stop();
    }

    return Response.json({
      request: count,
      remaining: Math.max(0, MAX_REQUESTS - count)
    });
  }
};
