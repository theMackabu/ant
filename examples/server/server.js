import meow from './meow.txt';
import { join } from 'ant:path';
import { html } from './html';
import { readFile } from 'ant:fs';
import { createRouter, addRoute, findRoute } from '../rou3';

const router = createRouter();

addRoute(router, 'GET', '/', c => c.res.body(`Welcome to Ant ${Ant.version}!`));

addRoute(router, 'GET', '/meow', async c => {
  const userAgent = c.req.header('User-Agent');
  c.res.header('X-Ant', 'meow');

  return c.res.body(`${meow}\n\n${userAgent}`);
});

addRoute(router, 'GET', '/echo', c =>
  fetch('http://localhost:8000/meow').then(res => {
    c.res.header('X-Ant', 'meow');
    c.res.body(res.body);
  })
);

addRoute(router, 'GET', '/get', c => {
  console.log(c.get('meow'));
  c.res.body(decodeURIComponent(c.get('meow')));
});

addRoute(router, 'GET', '/set/**:val', c => {
  c.set('meow', c.params.val);
  c.res.body(`meow = ${c.params.val}`);
});

addRoute(router, 'GET', '/fs/meow', async c => {
  const file = await readFile(join(import.meta.dirname, 'meow.txt'));
  return c.res.body(file || 'none');
});

addRoute(router, 'GET', '/hello', async c => {
  return c.res.body('Hello, World!');
});

addRoute(router, 'GET', '/status', async c => {
  await new Promise(resolve => setTimeout(resolve, 1000));
  const result = await Promise.resolve('Hello');
  return c.res.body(`server is responding with ${result}`);
});

addRoute(router, 'POST', '/users/:id', async c => {
  return c.res.body(`User ID: ${c.params.id}`);
});

addRoute(router, 'GET', '/users/:id/posts', async c => {
  return c.res.body(`Posts for user: ${c.params.id}`);
});

addRoute(router, 'GET', '/api/v1/users', async c => {
  return c.res.json({ users: [] });
});

addRoute(router, 'GET', '/zen', async c => {
  const response = await fetch('https://api.github.com/zen');
  return c.res.body(response.body);
});

addRoute(router, 'GET', '/api/v2/demo', async c => {
  const data = await fetch('https://themackabu.dev/test.json');
  return c.res.json(data.json());
});

addRoute(router, 'GET', '/files/**:path', async c => {
  return c.res.html(html`<div>${c.params.path}</div>`);
});

async function handleRequest(c) {
  console.log('request:', c.req.method, c.req.uri);
  const result = findRoute(router, c.req.method, c.req.uri);

  if (result?.data) {
    c.params = result.params;
    return await result.data(c);
  }

  c.res.body('not found: ' + c.req.uri, 404);
}

console.log('started on http://localhost:8000');
Ant.serve(8000, handleRequest);
