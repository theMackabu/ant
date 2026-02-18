let count = 0;

async function meow(c) {
  count++;
  c.res.body(`count is ${count}`);
}

async function server(c) {
  return await meow(c);
}

console.log('server on http://localhost:8000');
Ant.serve(8000, server);
