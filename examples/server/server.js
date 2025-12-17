import meow from './meow.txt';
import { join } from 'ant:path';
import { html } from './html';
import { readFile } from 'ant:fs';
import { Radix3 } from './radix3';

const router = new Radix3();

router.get('/', c => c.res.body(`Welcome to Ant ${Ant.version}!`));

router.get('/meow', async c => {
  const userAgent = c.req.header('User-Agent');
  c.res.header('X-Ant', 'meow');

  return c.res.body(`${meow}\n\n${userAgent}`);
});

router.get('/echo', c =>
  fetch('http://localhost:8000/meow').then(res => {
    c.res.header('X-Ant', 'meow');
    c.res.body(res.body);
  })
);

router.get('/get', c => {
  console.log(c.get('meow'));
  c.res.body(c.get('meow'));
});

router.get('/set/1', c => {
  c.set('meow', '1');
  c.res.body('meow = 1');
});

router.get('/set/2', c => {
  c.set('meow', '2');
  c.res.body('meow = 2');
});

router.get('/fs/meow', async c => {
  const file = await readFile(join(import.meta.dirname, 'meow.txt'));
  return c.res.body(file || 'none');
});

router.get('/hello', async c => {
  return c.res.body('Hello, World!');
});

router.get('/status', async c => {
  await new Promise(resolve => setTimeout(resolve, 1000));
  const result = await Promise.resolve('Hello');
  return c.res.body(`server is responding with ${result}`);
});

router.post('/users/:id', async c => {
  return c.res.body(`User ID: ${c.params.id}`);
});

router.get('/users/:id/posts', async c => {
  return c.res.body(`Posts for user: ${c.params.id}`);
});

router.get('/api/v1/users', async c => {
  return c.res.json({ users: [] });
});

router.get('/zen', async c => {
  const response = await fetch('https://api.github.com/zen');
  return c.res.body(response.body);
});

router.get('/api/v2/demo', async c => {
  const data = await fetch('https://themackabu.dev/test.json');
  return c.res.json(data.json());
});

router.get('/files/*path', async c => {
  return c.res.html(html`<div>${c.params.path}</div>`);
});

router.printTree();
console.log('');

async function handleRequest(c) {
  console.log('request:', c.req.method, c.req.uri);
  const result = router.lookup(c.req.uri, c.req.method);

  if (result?.handler) {
    c.params = result.params;
    return await result.handler(c);
  }

  c.res.body('not found: ' + c.req.uri, 404);
}

console.log('started on http://localhost:8000');
Ant.serve(8000, handleRequest);
