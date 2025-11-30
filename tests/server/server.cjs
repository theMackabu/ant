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

router.insert('/hello', () => {
  return { status: 200, body: 'Hello, World!' };
});

router.insert('/status', () => {
  return { status: 200, body: 'Server is running with Radix3 router!' };
});

router.insert('/echo', () => {
  return { status: 200, body: 'Echo endpoint - returns request details' };
});

router.insert('/users/:id', p => {
  return { status: 200, body: 'User ID: ' + p.id };
});

router.insert('/users/:id/posts', () => {
  return { status: 200, body: 'Posts for user: ' + p.id };
});

router.insert('/api/v1/users', () => {
  return { status: 200, body: 'API v1 users endpoint' };
});

router.insert('/api/v2/users', () => {
  return { status: 200, body: 'API v2 users endpoint' };
});

router.insert('/files/*path', () => {
  return { status: 200, body: 'File path: ' + p.path };
});

router.printTree();
Ant.println('');

function handleRequest(req, _res) {
  Ant.println('request:', req.method, req.uri);

  let result = router.lookup(req.uri);
  if (result !== undefined) return result;

  return { status: 404, body: 'Not Found: ' + req.uri };
}

Ant.println('started on http://localhost:8000');
Ant.serve(8000, handleRequest);
