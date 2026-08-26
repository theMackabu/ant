const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');

const sourceAnt = path.resolve(process.execPath);
const executable = fs.readFileSync(sourceAnt);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-upgrade-replace-'));
const home = path.join(root, 'home');
const cache = path.join(root, 'cache');
const bin = process.platform === 'win32'
  ? path.join(home, '.ant', 'bin')
  : path.join(home, '.local', 'bin');
const installedAnt = path.join(bin, process.platform === 'win32' ? 'ant.exe' : 'ant');

fs.mkdirSync(bin, { recursive: true });
fs.copyFileSync(sourceAnt, installedAnt);
if (process.platform !== 'win32') fs.chmodSync(installedAnt, 0o755);

const targets = [
  'darwin-aarch64',
  'darwin-x64',
  'linux-aarch64',
  'linux-aarch64-musl',
  'linux-x64',
  'linux-x64-musl',
  'windows-x64',
];

const server = http.createServer((req, res) => {
  if (req.url === '/ant') {
    res.writeHead(200, {
      'content-type': 'application/octet-stream',
      'content-length': executable.length,
      connection: 'close',
    });
    res.end(executable);
    return;
  }

  const body = JSON.stringify({
    ant: targets.map(target => ({
      target,
      available: true,
      version: '999.0.fixture.0',
      download_url: `http://127.0.0.1:${server.address().port}/ant`,
      build_timestamp: Math.floor(Date.now() / 1000) + 1,
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
    const child = spawn(installedAnt, args, {
      env: {
        ...process.env,
        HOME: home,
        USERPROFILE: home,
        XDG_CACHE_HOME: cache,
        ANT_MANIFEST_URL: `http://127.0.0.1:${server.address().port}/manifest`,
        ...extraEnv,
      },
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
    }, 10000);
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

  try {
    let result = await runAnt(['--no-color', 'upgrade']);
    assert.strictEqual(result.timedOut, false);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.match(result.stdout, /Upgraded successfully to Ant 999\.0\.fixture\.0/);
    assert.strictEqual(fs.existsSync(installedAnt), true);

    result = await runAnt(['--no-color', 'upgrade']);
    assert.strictEqual(result.timedOut, false);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.match(result.stdout, /Upgraded successfully to Ant 999\.0\.fixture\.0/);

    result = await runAnt(['--version-raw'], { ANT_NO_VERSION_CHECK: '1' });
    assert.strictEqual(result.timedOut, false);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(result.stdout.trim(), process.versions.ant);

    console.log('upgrade replace ok');
  } finally {
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
