const assert = require('node:assert');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');

const sourceAnt = path.resolve(process.execPath);
const executable = fs.readFileSync(sourceAnt);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-upgrade-mirror-'));
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

const requests = [];
let base = '';

function send(res, status, headers, body = '') {
  res.writeHead(status, { 'content-length': Buffer.byteLength(body), connection: 'close', ...headers });
  res.end(body);
}

const server = http.createServer((req, res) => {
  requests.push(req.url);
  switch (req.url) {
    case '/manifest':
      return send(res, 503, {}, 'unavailable');
    case '/latest/download/latest.json':
      return send(res, 302, { location: '../assets/latest.json' });
    case '/assets/latest.json':
      return send(res, 200, { 'content-type': 'application/json' }, JSON.stringify({
        schema: 1,
        ant: targets.map(target => ({
          target,
          available: true,
          version: '999.0.fixture.0',
          build_timestamp: null,
          download_url: `${base}/download/ant`,
        })),
      }));
    case '/download/ant':
      return send(res, 301, { location: `${base}/assets/ant` });
    case '/assets/ant':
      res.writeHead(200, {
        'content-type': 'application/octet-stream',
        'content-length': executable.length,
        connection: 'close',
      });
      return res.end(executable);
    case '/loop':
      return send(res, 302, { location: '/loop' });
    default:
      return send(res, 404, {}, 'missing');
  }
});

function runAnt(args, extraEnv) {
  return new Promise((resolve, reject) => {
    const child = spawn(installedAnt, args, {
      env: {
        ...process.env,
        HOME: home,
        USERPROFILE: home,
        XDG_CACHE_HOME: cache,
        ANT_NO_VERSION_CHECK: '1',
        ANT_MANIFEST_URL: `${base}/manifest`,
        ...extraEnv,
      },
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let output = '';
    child.stdout.on('data', chunk => { output += String(chunk); });
    child.stderr.on('data', chunk => { output += String(chunk); });
    child.once('error', reject);
    const timer = setTimeout(() => child.kill('SIGKILL'), 10000);
    child.once('exit', status => {
      clearTimeout(timer);
      resolve({ status, output });
    });
  });
}

async function check(args, mirror, expectedStatus, pattern, expectedRequests) {
  requests.length = 0;
  const env = mirror === undefined ? {} : { ANT_RELEASE_MIRROR_URL: mirror };
  const result = await runAnt(['--no-color', 'upgrade', ...args], env);
  assert.strictEqual(result.status, expectedStatus, result.output);
  assert.match(result.output, pattern);
  assert.deepStrictEqual(requests, expectedRequests);
}

async function main() {
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  base = `http://127.0.0.1:${server.address().port}`;

  try {
    await check(['--stable'], `${base}/latest/download/latest.json`, 0, /Upgraded successfully to Ant 999\.0\.fixture\.0/, [
      '/manifest',
      '/latest/download/latest.json',
      '/assets/latest.json',
      '/download/ant',
      '/assets/ant',
    ]);
    assert.strictEqual(fs.readFileSync(installedAnt).equals(executable), true);

    await check(['--stable'], undefined, 1, /HTTP 503/, ['/manifest']);
    await check(['--stable'], '', 1, /HTTP 503/, ['/manifest']);
    await check(['--canary'], `${base}/latest/download/latest.json`, 1, /HTTP 503/, ['/manifest?channel=canary']);
    await check(['--stable'], `${base}/missing`, 1, /HTTP 503.*GitHub fallback:.*HTTP 404/, ['/manifest', '/missing']);
    await check(['--stable'], `${base}/loop`, 1, /too many redirects/, ['/manifest', ...Array(6).fill('/loop')]);
    await check(['--stable'], `https://127.0.0.1:1/latest.json`, 1, /GitHub fallback/, ['/manifest']);

    console.log('upgrade mirror ok');
  } finally {
    await new Promise(resolve => server.close(resolve));
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
