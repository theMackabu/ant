import { open } from 'ant:fs';
import { join } from 'ant:path';
import { Radix3 } from '../server/radix3';

const router = new Radix3();

router.get('*path', c => {
  const path = c.params.path === '/' ? 'index.html' : c.params.path;
  const file = open(join(import.meta.dirname, path));

  if (path.endsWith('.html')) {
    return c.res.html(file);
  }

  if (path.endsWith('.js')) {
    return c.res.body(file, 200, 'application/javascript');
  }

  if (path.endsWith('.png')) {
    return c.res.body(file, 200, 'image/png');
  }

  if (path.endsWith('.jpg') || path.endsWith('.jpeg')) {
    return c.res.body(file, 200, 'image/jpeg');
  }

  if (path.endsWith('.gif')) {
    return c.res.body(file, 200, 'image/gif');
  }

  if (path.endsWith('.svg')) {
    return c.res.body(file, 200, 'image/svg+xml');
  }

  if (path.endsWith('.ico')) {
    return c.res.body(file, 200, 'image/x-icon');
  }

  return c.res.body(file);
});

router.get('/api/version', async c => c.res.json({ version: Ant.version }));

router.printTree();
console.log('');

async function handleRequest(req, res) {
  console.log('request:', req.method, req.uri);
  const result = router.lookup(req.uri, req.method);

  if (result?.handler) {
    const ctx = { req, res, params: result.params };
    return await result.handler(ctx);
  }

  res.body('not found: ' + req.uri, 404);
}

console.log('started on http://localhost:8000');
Ant.serve(8000, handleRequest);
