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

const currentVersion = process.versions.ant;
const currentTimestamp = Number(Ant.buildDate);
const isCanary = Ant.channel === 'canary';
assert.strictEqual(typeof Ant.version, 'string');
assert.strictEqual(Ant.version, currentVersion);
assert.match(currentVersion, /^\d+\.\d+\.[^.]+\.\d+$/);
const stableVersion = currentVersion;
const canaryVersion = currentVersion;
const requests = [];
let stableRelease = { version: '999.0.fixture.0', build_timestamp: currentTimestamp - 1 };
let canaryRelease = { version: '999.0.fixture.0', build_timestamp: currentTimestamp + 1 };
let stableMissing = false;
let canaryMissing = false;

const server = http.createServer((req, res) => {
  requests.push(req.url);
  if (req.url === '/ant' || req.url === '/ant-canary') {
    res.writeHead(200, {
      'content-type': 'application/octet-stream',
      'content-length': executable.length,
      connection: 'close',
    });
    res.end(executable);
    return;
  }

  const canary = req.url.includes('channel=canary');
  if (canary ? canaryMissing : stableMissing) {
    res.writeHead(404, { connection: 'close' });
    res.end('missing');
    return;
  }

  const body = JSON.stringify({
    ant: targets.map(target => ({
      target,
      available: true,
      ...(canary ? canaryRelease : stableRelease),
      download_url: `http://127.0.0.1:${server.address().port}/${canary ? 'ant-canary' : 'ant'}`,
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

async function checkUpgrade(flags, expectedRequests, message, status = 0) {
  requests.length = 0;
  const result = await runAnt(['--no-color', 'upgrade', ...flags]);
  assert.strictEqual(result.timedOut, false);
  assert.strictEqual(result.status, status, result.stderr);
  assert.match(result.stdout + result.stderr, message);
  assert.deepStrictEqual(requests, expectedRequests);
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

    result = await runAnt(['--version-raw'], {
      ANT_NO_VERSION_CHECK: '1', ANT_CANARY: isCanary ? '0' : '1',
    });
    assert.strictEqual(result.timedOut, false);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(result.stdout, `${currentVersion}\n`);

    const beforeChannelRequests = requests.length;
    result = await runAnt(['--version-channel'], { ANT_CANARY: isCanary ? '0' : '1' });
    assert.strictEqual(result.timedOut, false);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(result.stdout, `${Ant.channel}\n`);
    assert.strictEqual(requests.length, beforeChannelRequests, 'channel lookup should not use the network');

    result = await runAnt(['--no-color', '-e', 'console.log(Ant.version)']);
    assert.strictEqual(result.status, 0, result.stderr);
    assert.strictEqual(result.stdout, `${currentVersion}\n`);

    result = await runAnt(['--no-color', '--version'], { ANT_NO_VERSION_CHECK: '1' });
    assert.strictEqual(result.status, 0, result.stderr);
    assert.ok(result.stdout.includes(`${currentVersion}${isCanary ? ' (canary)' : ''} (released `));

    await checkUpgrade([], ['/manifest', '/ant'], /Upgraded successfully/);
    await checkUpgrade(['--canary'], ['/manifest?channel=canary', '/ant-canary'], /Upgraded successfully.*\(canary\)/);
    await checkUpgrade(['--canary', '--stable'], [], /cannot be combined/, 1);

    stableRelease = { version: stableVersion, build_timestamp: currentTimestamp };
    canaryRelease = { version: canaryVersion, build_timestamp: currentTimestamp };
    await checkUpgrade(['--stable'], isCanary ? ['/manifest', '/ant'] : ['/manifest'],
      isCanary ? /Upgraded successfully/ : /already up to date/);
    await checkUpgrade(['--canary'], isCanary ? ['/manifest?channel=canary'] : ['/manifest?channel=canary', '/ant-canary'],
      isCanary ? /already up to date/ : /Upgraded successfully/);
    await checkUpgrade([], isCanary ? ['/manifest', '/manifest?channel=canary'] : ['/manifest'], /already up to date/);

    stableRelease = { version: '0.0.00000000.0', build_timestamp: currentTimestamp + 1 };
    await checkUpgrade(['--stable'], ['/manifest', '/ant'], /Downgraded successfully/);
    await checkUpgrade([], isCanary ? ['/manifest', '/manifest?channel=canary'] : ['/manifest'], /already up to date/);

    // Same release, different commit: timestamps decide.
    const parts = stableVersion.split('.');
    parts[2] = parts[2] === 'ffffffff' ? '00000000' : 'ffffffff';
    canaryRelease = { version: parts.join('.'), build_timestamp: currentTimestamp + 1 };
    await checkUpgrade([], isCanary ? ['/manifest', '/manifest?channel=canary', '/ant-canary'] : ['/manifest'],
      isCanary ? /Upgraded successfully.*\(canary\)/ : /already up to date/);

    canaryRelease.build_timestamp = currentTimestamp - 1;
    await checkUpgrade([], isCanary ? ['/manifest', '/manifest?channel=canary'] : ['/manifest'], /already up to date/);
    await checkUpgrade(['--canary'], ['/manifest?channel=canary', '/ant-canary'], /Downgraded successfully/);

    if (isCanary) {
      stableRelease = { version: stableVersion, build_timestamp: currentTimestamp + 1 };
      await checkUpgrade([], ['/manifest', '/ant'], /Upgraded successfully/);

      stableMissing = true;
      canaryRelease = { version: canaryVersion, build_timestamp: currentTimestamp };
      await checkUpgrade([], ['/manifest', '/manifest?channel=canary'], /already up to date/);
      canaryMissing = true;
      await checkUpgrade([], ['/manifest', '/manifest?channel=canary'], /no canary build has been published/, 1);
    }

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
