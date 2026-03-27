let count = 0;

async function meow() {
  count++;
  return new Response(`count is ${count}`);
}

async function server() {
  return await meow();
}

console.log('server on http://localhost:3000');
export default { fetch: server };
