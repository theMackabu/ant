const assert = require('node:assert');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-multipart-publish-'));
const home = path.join(tmp, 'home');
const pkg = path.join(tmp, 'pkg');
fs.mkdirSync(home);
fs.mkdirSync(pkg);
fs.writeFileSync(path.join(pkg, 'package.json'), JSON.stringify({ name: 'multipart-fixture', version: '1.2.3' }));

// Incompressible data keeps the .tgz over the mock registry's 5 MiB part size,
// proving that the native client sends multiple bounded raw requests.
const payload = fs.openSync(path.join(pkg, 'native.bin'), 'w');
for (let i = 0; i < 192; i++) fs.writeSync(payload, crypto.randomBytes(64 * 1024));
fs.closeSync(payload);

let declared;
let uploaded = 0;
let expectedPart = 1;
let completed = false;
let finalized = false;
const sha1 = crypto.createHash('sha1');
const sha512 = crypto.createHash('sha512');

function json(res, status, body) {
  const text = JSON.stringify(body);
  res.writeHead(status, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(text) });
  res.end(text);
}

const server = http.createServer((req, res) => {
  assert.strictEqual(req.headers.authorization, 'Bearer fixture-token');
  const chunks = [];
  req.on('data', chunk => chunks.push(chunk));
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    if (req.method === 'POST' && req.url === '/-/v1/publish/uploads') {
      declared = JSON.parse(body.toString());
      assert.strictEqual(declared.name, 'multipart-fixture');
      assert.strictEqual(declared.version, '1.2.3');
      return json(res, 201, { id: 'fixture-upload', partSize: 5 * 1024 * 1024 });
    }
    const part = req.url.match(/^\/-\/v1\/publish\/uploads\/fixture-upload\/parts\/(\d+)$/);
    if (req.method === 'PUT' && part) {
      assert.strictEqual(Number(part[1]), expectedPart++);
      assert(body.length <= 5 * 1024 * 1024);
      uploaded += body.length;
      sha1.update(body);
      sha512.update(body);
      return json(res, 200, { partNumber: Number(part[1]), etag: `part-${part[1]}` });
    }
    if (req.method === 'POST' && req.url === '/-/v1/publish/uploads/fixture-upload/complete') {
      const complete = JSON.parse(body.toString());
      assert.strictEqual(complete.parts.length, expectedPart - 1);
      completed = true;
      return json(res, 200, { ok: true, complete: true });
    }
    if (req.method === 'POST' && req.url === '/-/v1/publish/uploads/fixture-upload/finalize') {
      const metadata = JSON.parse(body.toString());
      assert(completed);
      assert.strictEqual(metadata.versions['1.2.3'].name, 'multipart-fixture');
      assert.strictEqual(metadata._attachments, undefined);
      finalized = true;
      return json(res, 201, { ok: true, added: ['1.2.3'] });
    }
    json(res, 404, { error: 'unexpected request' });
  });
});

(async () => {
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    const port = server.address().port;
    fs.writeFileSync(path.join(home, '.npmrc'), '//127.0.0.1/:_authToken=fixture-token\n');
    const ant = process.env.ANT_TEST_BIN || path.resolve(process.execPath);
    const child = spawn(ant, ['publish'], {
      cwd: pkg,
      env: { ...process.env, HOME: home, ANTS_REGISTRY: `http://127.0.0.1:${port}` },
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', chunk => (stdout += chunk));
    child.stderr.on('data', chunk => (stderr += chunk));
    const status = await new Promise((resolve, reject) => {
      child.once('error', reject);
      child.once('exit', resolve);
    });
    assert.strictEqual(status, 0, `ant publish failed\nstdout:\n${stdout}\nstderr:\n${stderr}`);
    assert(finalized);
    assert(expectedPart > 2, 'expected more than one upload part');
    assert.strictEqual(uploaded, declared.size);
    assert.strictEqual(sha1.digest('hex'), declared.shasum);
    assert.strictEqual(`sha512-${sha512.digest('base64')}`, declared.integrity);
    assert.match(stdout, /Published multipart-fixture@1\.2\.3/);
    console.log('native registry multipart publish protocol ok');
  } finally {
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(tmp, { recursive: true, force: true });
  }
})().catch(err => {
  console.error(err);
  process.exitCode = 1;
});
