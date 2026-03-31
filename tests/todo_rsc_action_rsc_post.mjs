function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

const { default: handler } = await import('../todo/rsc/dist/rsc/index.js');

const response = await handler.fetch(
  new Request('http://localhost/_.rsc', {
    method: 'POST',
    headers: {
      'x-rsc-action': '710363d987f5#updateServerCounter',
    },
    body: '[1]',
  }),
);

console.log(`status:${response.status}`);
console.log(`content-type:${response.headers.get('content-type') || 'missing'}`);

assert(
  response.headers.get('content-type') === 'text/x-component;charset=utf-8',
  'expected RSC action response content-type'
);
