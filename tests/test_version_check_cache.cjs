const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');

const ant = path.resolve(process.execPath);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-version-check-'));
const home = path.join(root, 'home');
const xdgCache = path.join(root, 'cache');
fs.mkdirSync(home, { recursive: true });

const targets = [
  'darwin-aarch64',
  'darwin-x64',
  'linux-aarch64',
  'linux-aarch64-musl',
  'linux-x64',
  'linux-x64-musl',
  'windows-x64',
];

let requestCount = 0;
let shouldFail = false;
let latestVersion = '999.0.fixture.0';

const server = http.createServer((req, res) => {
  requestCount++;
  if (shouldFail) {
    req.socket.destroy();
    return;
  }

  const body = JSON.stringify({
    ant: targets.map(target => ({
      target,
      available: true,
      version: latestVersion,
      download_url: `http://127.0.0.1:${server.address().port}/ant`,
      build_timestamp: Math.floor(Date.now() / 1000),
    })),
  });
  res.writeHead(200, {
    'content-type': 'application/json',
    'content-length': Buffer.byteLength(body),
    connection: 'close',
  });
  res.end(body);
});

function runAnt(args, extraEnv = {}) {
  return new Promise((resolve, reject) => {
    const env = {
      ...process.env,
      HOME: home,
      USERPROFILE: home,
      XDG_CACHE_HOME: xdgCache,
      ANT_MANIFEST_URL: `http://127.0.0.1:${server.address().port}/manifest`,
      ...extraEnv,
    };
    delete env.ANT_NO_VERSION_CHECK;
    if (extraEnv.ANT_NO_VERSION_CHECK) env.ANT_NO_VERSION_CHECK = extraEnv.ANT_NO_VERSION_CHECK;

    const child = spawn(ant, args, {
      env,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    let timedOut = false;
    child.stdout.on('data', chunk => { stdout += String(chunk); });
    child.stderr.on('data', chunk => { stderr += String(chunk); });
    child.once('error', reject);
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill('SIGKILL');
    }, 5000);
    child.once('exit', (status, signal) => {
      clearTimeout(timer);
      resolve({ status, signal, stdout, stderr, timedOut });
    });
  });
}

async function main() {
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });

  const cacheFile = process.platform === 'win32'
    ? path.join(home, '.ant', 'version-check.json')
    : path.join(xdgCache, 'ant', 'version-check.json');

  try {
    let result = await runAnt(['--no-color', '--help']);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(result.timedOut, false);
    assert.match(result.stdout, /update available: 999\.0\.fixture\.0/);
    assert.strictEqual(requestCount, 1);
    assert.strictEqual(fs.existsSync(cacheFile), true);

    result = await runAnt(['--no-color', '--version']);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.match(result.stdout, /update available: 999\.0\.fixture\.0/);
    assert.strictEqual(requestCount, 1, 'fresh cache should avoid another manifest request');

    const stale = JSON.parse(fs.readFileSync(cacheFile, 'utf8'));
    stale.checked_at = Math.floor(Date.now() / 1000) - 73 * 60 * 60;
    fs.writeFileSync(cacheFile, JSON.stringify(stale));
    shouldFail = true;

    result = await runAnt(['--no-color', '--help']);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.match(result.stdout, /update available: 999\.0\.fixture\.0/);
    assert.strictEqual(requestCount, 2, 'stale cache should trigger one manifest request');

    result = await runAnt(['--no-color', '--version']);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.match(result.stdout, /update available: 999\.0\.fixture\.0/);
    assert.strictEqual(requestCount, 2, 'failed checks should also be throttled for 72 hours');

    result = await runAnt(['--no-color', '--help'], { ANT_NO_VERSION_CHECK: '1' });
    assert.strictEqual(result.status, 0, result.stderr);
    assert.doesNotMatch(result.stdout, /update available:/);
    assert.strictEqual(requestCount, 2, 'ANT_NO_VERSION_CHECK should bypass the cache and network');

    shouldFail = false;
    latestVersion = process.versions.ant;
    assert.strictEqual(typeof latestVersion, 'string');
    result = await runAnt(['upgrade']);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.match(result.stdout, /Ant is already up to date\./);
    assert.strictEqual(requestCount, 3, 'ant upgrade should always make a fresh request');

    result = await runAnt(['--no-color', '--help']);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.doesNotMatch(result.stdout, /update available:/);
    assert.strictEqual(requestCount, 3, 'ant upgrade should refresh the version cache');

    console.log('version check cache ok');
  } finally {
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
