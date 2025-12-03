import { html } from './html';
import meow from './meow.txt';
import { Radix3 } from './radix3';

const router = new Radix3();

router.insert('/', c => {
  return c.res.body(`Welcome to Ant HTTP Server with Radix3 Router!

Available routes:
  GET /
  GET /meow
  GET /hello
  GET /status
  GET /users/:id
  GET /users/:id/posts
  GET /files/*path
  GET /api/v1/users
  GET /api/v2/demo`);
});

router.insert('/meow', async c => {
  return c.res.body(meow);
});

router.insert('/hello', async c => {
  return c.res.body('Hello, World!');
});

router.insert('/status', async c => {
  await new Promise(resolve => setTimeout(resolve, 100));
  const result = await Promise.resolve('Hello');
  // const result = (await fetch('http://localhost:8000/meow')).text();
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

router.insert('/api/v2/demo', async c => {
  return c.res.json({
    slideshow: {
      author: 'Yours Truly',
      date: 'date of publication',
      slides: [
        {
          title: 'Wake up to WonderWidgets!',
          type: 'all'
        },
        {
          items: ['Why <em>WonderWidgets</em> are great', 'Who <em>buys</em> WonderWidgets'],
          title: 'Overview',
          type: 'all'
        }
      ],
      metadata: {
        title: 'Sample Slide Show',
        isFavorite: true,
        viewCount: 105407,
        createdAt: '2024-12-01T22:51:49.000Z'
      }
    }
  });
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
