import { Radix3 } from '../examples/server/radix3.js';

const router = new Radix3();

router.get('/', function () {
  return 'home';
});
router.get('/api', function () {
  return 'api';
});
router.get('/api/users', function () {
  return 'users';
});
router.get('/api/users/list', function () {
  return 'users list';
});
router.get('/api/posts', function () {
  return 'posts';
});
router.get('/api/posts/recent', function () {
  return 'recent posts';
});
router.get('/api/comments', function () {
  return 'comments';
});
router.get('/health', function () {
  return 'health';
});
router.get('/status', function () {
  return 'status';
});
router.get('/about', function () {
  return 'about';
});
router.get('/contact', function () {
  return 'contact';
});
router.get('/login', function () {
  return 'login';
});
router.get('/logout', function () {
  return 'logout';
});
router.get('/dashboard', function () {
  return 'dashboard';
});
router.get('/settings', function () {
  return 'settings';
});

// Parameterized routes
router.get('/api/users/:id', function () {
  return 'user by id';
});
router.get('/api/users/:id/posts', function () {
  return 'user posts';
});
router.get('/api/users/:id/comments', function () {
  return 'user comments';
});
router.get('/api/posts/:id', function () {
  return 'post by id';
});
router.get('/api/posts/:id/comments', function () {
  return 'post comments';
});
router.get('/api/posts/:postId/comments/:commentId', function () {
  return 'specific comment';
});
router.get('/users/:userId/profile', function () {
  return 'user profile';
});
router.get('/products/:category/:id', function () {
  return 'product';
});

router.get('/static/*path', function () {
  return 'static file';
});
router.get('/assets/*file', function () {
  return 'asset file';
});

router.post('/api/users', function () {
  return 'create user';
});
router.put('/api/users/:id', function () {
  return 'update user';
});
router.delete('/api/users/:id', function () {
  return 'delete user';
});
router.patch('/api/users/:id', function () {
  return 'patch user';
});

console.log('Router tree:');
router.printTree();
console.log('');

const ITERATIONS = 100000;

function bench(name, fn) {
  const start = performance.now();
  for (let i = 0; i < ITERATIONS; i = i + 1) {
    fn();
  }
  const end = performance.now();

  const totalMs = end - start;
  const opsPerSec = Math.round(ITERATIONS / (totalMs / 1000));
  const nsPerOp = Math.round((totalMs * 1000000) / ITERATIONS);

  console.log(name + ': ' + opsPerSec.toLocaleString() + ' ops/sec (' + nsPerOp + ' ns/op)');
}

console.log('--- Static Route Lookup (' + ITERATIONS + ' iterations) ---');
bench('Lookup "/" (root)', function () {
  router.lookup('/');
});
bench('Lookup "/api" (short)', function () {
  router.lookup('/api');
});
bench('Lookup "/api/users" (medium)', function () {
  router.lookup('/api/users');
});
bench('Lookup "/api/posts/recent" (long)', function () {
  router.lookup('/api/posts/recent');
});
bench('Lookup "/dashboard" (single segment)', function () {
  router.lookup('/dashboard');
});

console.log('\n--- Parameterized Route Lookup (' + ITERATIONS + ' iterations) ---');
bench('Lookup "/api/users/123"', function () {
  router.lookup('/api/users/123');
});
bench('Lookup "/api/users/456/posts"', function () {
  router.lookup('/api/users/456/posts');
});
bench('Lookup "/api/posts/789/comments/42"', function () {
  router.lookup('/api/posts/789/comments/42');
});
bench('Lookup "/products/electronics/12345"', function () {
  router.lookup('/products/electronics/12345');
});

console.log('\n--- Wildcard Route Lookup (' + ITERATIONS + ' iterations) ---');
bench('Lookup "/static/css/style.css"', function () {
  router.lookup('/static/css/style.css');
});
bench('Lookup "/assets/images/logo.png"', function () {
  router.lookup('/assets/images/logo.png');
});

console.log('\n--- Different HTTP Methods (' + ITERATIONS + ' iterations) ---');
bench('GET /api/users/:id', function () {
  router.lookup('/api/users/123', 'GET');
});
bench('POST /api/users', function () {
  router.lookup('/api/users', 'POST');
});
bench('PUT /api/users/:id', function () {
  router.lookup('/api/users/123', 'PUT');
});
bench('DELETE /api/users/:id', function () {
  router.lookup('/api/users/123', 'DELETE');
});

console.log('\n--- Not Found Routes (' + ITERATIONS + ' iterations) ---');
bench('Lookup "/notfound"', function () {
  router.lookup('/notfound');
});
bench('Lookup "/api/unknown/path"', function () {
  router.lookup('/api/unknown/path');
});

console.log('\n=== Benchmark Complete ===');
