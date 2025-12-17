import { open } from 'ant:fs';
import { join, extname } from 'ant:path';
import { Radix3 } from '../server/radix3';

const router = new Radix3();

const validPaths = new Set();
const invalidPaths = new Set();

const basePath = import.meta.dirname;
const indexPath = join(basePath, 'index.html');

const mimeTypes = new Map([
  ['.html', 'text/html'],
  ['.js', 'application/javascript'],
  ['.css', 'text/css'],
  ['.json', 'application/json'],
  ['.png', 'image/png'],
  ['.jpg', 'image/jpeg'],
  ['.jpeg', 'image/jpeg'],
  ['.gif', 'image/gif'],
  ['.svg', 'image/svg+xml'],
  ['.ico', 'image/x-icon'],
  ['.woff', 'font/woff'],
  ['.woff2', 'font/woff2']
]);

router.get('/api/version', async c => c.res.json({ version: Ant.version }));

router.get('*path', c => {
  const reqPath = c.params.path;
  if (reqPath === '/') return c.res.body(open(indexPath), 200, 'text/html');

  const filePath = reqPath === '/' ? indexPath : join(basePath, reqPath);

  if (validPaths.has(filePath)) {
    const ext = extname(reqPath) || '.html';
    return c.res.body(open(filePath), 200, mimeTypes.get(ext) ?? 'application/octet-stream');
  }

  if (invalidPaths.has(filePath)) {
    return c.res.body(open(indexPath), 200, 'text/html');
  }

  try {
    const file = open(filePath);
    validPaths.add(filePath);

    const ext = extname(reqPath) || '.html';
    return c.res.body(file, 200, mimeTypes.get(ext) ?? 'application/octet-stream');
  } catch {
    invalidPaths.add(filePath);
    return c.res.body(open(indexPath), 200, 'text/html');
  }
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
