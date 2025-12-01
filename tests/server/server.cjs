const Radix3 = Ant.require('./radix3.cjs');
const router = new Radix3();

router.insert('/', () => {
  return {
    status: 200,
    body: `Welcome to Ant HTTP Server with Radix3 Router!
  
Available routes:
  GET /
  GET /hello
  GET /status
  GET /echo
  GET /users/:id
  GET /users/:id/posts
  GET /files/*path
  GET /api/v1/users
  GET /api/v2/users`
  };
});

router.insert('/hello', async () => {
  return { status: 200, body: 'Hello, World!' };
});

router.insert('/status', async () => {
  return { status: 200, body: 'Server is running with Radix3 router!' };
});

router.insert('/echo', async () => {
  return { status: 200, body: 'Echo endpoint - returns request details' };
});

router.insert('/users/:id', async p => {
  return { status: 200, body: 'User ID: ' + p.id };
});

router.insert('/users/:id/posts', async () => {
  return { status: 200, body: 'Posts for user: ' + p.id };
});

router.insert('/api/v1/users', async () => {
  return { status: 200, body: 'API v1 users endpoint' };
});

router.insert('/api/v2/users', async () => {
  return { status: 200, body: 'API v2 users endpoint' };
});

router.insert('/files/*path', async () => {
  return { status: 200, body: 'File path: ' + p.path };
});

router.printTree();
Ant.println('');

async function handleRequest(req, _res) {
  Ant.println('request:', req.method, req.uri);

  const { handler, params } = router.lookup(req.uri);
  handler(params);

  return { status: 404, body: 'Not Found: ' + req.uri };
}

Ant.println('started on http://localhost:8000');
Ant.serve(8000, handleRequest);
