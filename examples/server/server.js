import meow from './meow.txt';
import { join } from 'ant:path';
import { html } from './html';
import { readFile } from 'ant:fs';
import { createRouter, addRoute, findRoute } from '../rou3';

const router = createRouter();

addRoute(router, 'GET', '/', () => {
  return new Response(`Welcome to Ant ${Ant.version}!`);
});

addRoute(router, 'GET', '/meow', req => {
  const userAgent = req.headers.get('User-Agent');
  return new Response(`${meow}\n\n${userAgent}`, {
    headers: { 'X-Ant': 'meow' }
  });
});

addRoute(router, 'GET', '/echo', async () => {
  const res = await fetch('http://localhost:8000/meow');
  return new Response(await res.text(), {
    headers: { 'X-Ant': 'meow' }
  });
});

addRoute(router, 'GET', '/fs/meow', async () => {
  const file = await readFile(join(import.meta.dirname, 'meow.txt'));
  return new Response(file || 'none');
});

addRoute(router, 'GET', '/hello', () => {
  return new Response('Hello, World!');
});

addRoute(router, 'GET', '/status', async () => {
  await new Promise(resolve => setTimeout(resolve, 1000));
  const result = await Promise.resolve('Hello');
  return new Response(`server is responding with ${result}`);
});

addRoute(router, 'POST', '/users/:id', (_req, params) => {
  return new Response(`User ID: ${params.id}`);
});

addRoute(router, 'GET', '/users/:id/posts', (_req, params) => {
  return new Response(`Posts for user: ${params.id}`);
});

addRoute(router, 'GET', '/api/v1/users', () => {
  return Response.json({ users: [] });
});

addRoute(router, 'GET', '/zen', async () => {
  const response = await fetch('https://api.github.com/zen');
  return new Response(await response.text());
});

addRoute(router, 'GET', '/api/v2/demo', async () => {
  const data = await fetch('https://themackabu.dev/test.json');
  return Response.json(await data.json());
});

addRoute(router, 'GET', '/files/**:path', (_req, params) => {
  return new Response(html`<div>${params.path}</div>`, {
    headers: { 'Content-Type': 'text/html' }
  });
});

console.log('started on http://localhost:8000');

export default {
  port: 8000,
  fetch(req) {
    const url = new URL(req.url);

    console.log('request:', req.method, url.pathname);
    const result = findRoute(router, req.method, url.pathname);

    if (result?.data) return result.data(req, result.params);
    return new Response('not found: ' + url.pathname, { status: 404 });
  }
};
