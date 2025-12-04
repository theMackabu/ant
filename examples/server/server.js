import meow from './meow.txt';

import { html } from './html';
import { readFile } from 'ant:fs';
import { Radix3 } from './radix3';

const router = new Radix3();

router.insert('/', c => c.res.body(`Welcome to Ant ${Ant.version}!`));

router.insert('/meow', async c => {
  return c.res.body(meow);
});

router.insert('/fs/meow', async c => {
  const file = await readFile('./meow.txt');
  return c.res.body(file);
});

router.insert('/hello', async c => {
  return c.res.body('Hello, World!');
});

router.insert('/status', async c => {
  await new Promise(resolve => setTimeout(resolve, 1000));
  const result = await Promise.resolve('Hello');
  return c.res.body(`server is responding with ${result}`);
});

router.insert('/users/:id', async c => {
  return c.res.body(`User ID: ${c.params.id}`);
});

router.insert('/users/:id/posts', async c => {
  return c.res.body(`Posts for user: ${c.params.id}`);
});

router.insert('/api/v1/users', async c => {
  return c.res.json({ users: [] });
});

router.insert('/zen', async c => {
  const response = await fetch('https://api.github.com/zen');
  return c.res.body(response.body);
});

router.insert('/api/v2/demo', async c => {
  const data = await fetch('https://themackabu.dev/test.json');
  return c.res.json(data.json());
});

router.insert('/files/*path', async c => {
  return c.res.html(html`<div>${c.params.path}</div>`);
});

router.printTree();
console.log('');

async function handleRequest(req, res) {
  console.log('request:', req.method, req.uri);
  const result = router.lookup(req.uri);

  if (result?.handler) {
    const ctx = { req, res, params: result.params };
    return await result.handler(ctx);
  }

  res.body('not found: ' + req.uri, 404);
}

console.log('started on http://localhost:8000');
Ant.serve(8000, handleRequest);
