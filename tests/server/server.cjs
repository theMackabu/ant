const Radix3 = Ant.require('./radix3.cjs');
const { html } = Ant.require('./html.cjs');

const router = new Radix3();

router.insert('/', c => {
  return c.res.body(`Welcome to Ant HTTP Server with Radix3 Router!

Available routes:
  GET /
  GET /hello
  GET /status
  GET /users/:id
  GET /users/:id/posts
  GET /files/*path
  GET /api/v1/users
  GET /api/v2/users`);
});

router.insert('/hello', async c => {
  return c.res.body('Hello, World!');
});

router.insert('/status', async c => {
  return c.res.body('Server is running with Radix3 router!');
});

router.insert('/users/:id', async c => {
  return c.res.body(`User ID: ${c.params.id}`);
});

router.insert('/users/:id/posts', async c => {
  return c.res.body(`Posts for user: ${c.params.id}`);
});

router.insert('/api/v1/users', async c => {
  return c.res.json({ users: null });
});

router.insert('/api/v2/users', async c => {
  return c.res.json({ users: [] });
});

router.insert('/files/*path', async c => {
  return c.res.html(html`<div>${c.params.path}</div>`);
});

router.printTree();
Ant.println('');

async function handleRequest(req, res) {
  Ant.println('request:', req.method, req.uri);
  const result = router.lookup(req.uri);

  if (result?.handler) {
    const ctx = { req, res, params: result.params };
    return void result.handler(ctx);
  }

  res.body('not found: ' + req.uri, 404);
}

Ant.println('started on http://localhost:8000');
Ant.serve(8000, handleRequest);
