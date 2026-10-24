const assert = require('node:assert');
const { spawn } = require('node:child_process');
const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');

const ant = path.resolve(process.execPath);
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-upgrade-github-'));
const home = path.join(root, 'home');
const cache = path.join(root, 'cache');
const bin = process.platform === 'win32'
  ? path.join(home, '.ant', 'bin')
  : path.join(home, '.local', 'bin');
const installedName = process.platform === 'win32' ? 'ant.exe' : 'ant';
const installedAnt = path.join(bin, installedName);
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

const currentVersion = process.versions.ant;
const isCanary = Ant.channel === 'canary';
const apiPath = '/api/repos/theMackabu/ant/releases/latest';
const tls = name => fs.readFileSync(path.join(__dirname, 'fixtures', 'tls', name));
// macOS and Windows verify TLS against the OS trust store, so SSL_CERT_FILE only works on Linux
const httpsCase = process.platform === 'linux';

// Larger than the reader's 32 KiB output buffer, and partly compressible.
const payload = Buffer.concat([
  crypto.randomBytes(48 * 1024),
  Buffer.from('ant upgrade github fixture\n'.repeat(8192)),
]);
const edgePayload = Buffer.from('abcdefghij'.repeat(3278)).subarray(0, 32773);

// Release assets set flag 0x08: zeroed local CRC and sizes, then a data descriptor.
function makeZip(entries, { method = 8, descriptor = true, badCrc = false } = {}) {
  const parts = [];
  const central = [];
  let offset = 0;
  for (const { name, data } of entries) {
    const nameBuf = Buffer.from(name);
    const body = method === 8 ? zlib.deflateRawSync(data) : data;
    const crc = (zlib.crc32(data) ^ (badCrc ? 1 : 0)) >>> 0;
    const flags = descriptor ? 0x08 : 0;
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(flags, 6);
    local.writeUInt16LE(method, 8);
    if (!descriptor) {
      local.writeUInt32LE(crc, 14);
      local.writeUInt32LE(body.length, 18);
      local.writeUInt32LE(data.length, 22);
    }
    local.writeUInt16LE(nameBuf.length, 26);
    const trailer = Buffer.alloc(descriptor ? 16 : 0);
    if (descriptor) {
      trailer.writeUInt32LE(0x08074b50, 0);
      trailer.writeUInt32LE(crc, 4);
      trailer.writeUInt32LE(body.length, 8);
      trailer.writeUInt32LE(data.length, 12);
    }

    const entry = Buffer.alloc(46);
    entry.writeUInt32LE(0x02014b50, 0);
    entry.writeUInt16LE(20, 4);
    entry.writeUInt16LE(20, 6);
    entry.writeUInt16LE(flags, 8);
    entry.writeUInt16LE(method, 10);
    entry.writeUInt32LE(crc, 16);
    entry.writeUInt32LE(body.length, 20);
    entry.writeUInt32LE(data.length, 24);
    entry.writeUInt16LE(nameBuf.length, 28);
    entry.writeUInt32LE(offset, 42);

    parts.push(local, nameBuf, body, trailer);
    central.push(entry, nameBuf);
    offset += local.length + nameBuf.length + body.length + trailer.length;
  }

  const directory = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...parts, directory, end]);
}

const sha256 = data => crypto.createHash('sha256').update(data).digest('hex');
const defaults = {
  tag: 'v999.0.fixture.0',
  apiStatus: 200,
  payload,
  zip: {},
  extraEntry: false,
  digest: 'real',
  shortSize: false,
  route: 'download',
  secure: false,
  manifest: 'down',
};
let state;
let zip;
const requests = [];

function reset(overrides = {}) {
  state = { ...defaults, ...overrides };
  const entries = [{ name: installedName, data: state.payload }];
  if (state.extraEntry) entries.push({ name: 'README', data: Buffer.from('extra\n') });
  zip = makeZip(entries, state.zip);
  requests.length = 0;
}

const digests = {
  real: () => `sha256:${sha256(zip)}`,
  tampered: () => `sha256:${sha256(Buffer.from('tampered'))}`,
  upper: () => `sha256:${sha256(zip).toUpperCase()}`,
  empty: () => '',
  missing: () => undefined,
};

function releaseJson() {
  const base = `http://127.0.0.1:${server.address().port}`;
  const assetBase = state.secure ? `https://127.0.0.1:${secureServer.address().port}` : base;
  return {
    tag_name: state.tag,
    html_url: `${base}/releases/tag/${state.tag}`,
    assets: targets.map(target => ({
      name: `ant-${target}.zip`,
      size: state.shortSize ? zip.length - 1 : zip.length,
      browser_download_url: `${assetBase}/${state.route}/ant-${target}.zip`,
      digest: digests[state.digest](),
    })),
  };
}

function send(res, status, headers, body = '') {
  res.writeHead(status, { connection: 'close', 'content-length': Buffer.byteLength(body), ...headers });
  res.end(body);
}

function handle(req, res) {
  const url = req.url;
  const base = `http://127.0.0.1:${server.address().port}`;
  requests.push({ url, headers: req.headers });

  if (url.startsWith('/manifest')) {
    if (state.manifest === 'down') return req.socket.destroy();
    return send(res, 200, { 'content-type': 'application/json' }, JSON.stringify({
      ant: targets.map(target => ({
        target,
        available: true,
        version: '999.0.fixture.0',
        build_timestamp: Number(Ant.buildDate) + 1,
        download_url: `${base}/blocked-download`,
      })),
    }));
  }
  if (url === '/blocked-download') return req.socket.destroy();
  if (url === apiPath) {
    if (state.apiStatus !== 200) return send(res, state.apiStatus, {}, '{"message":"refused"}');
    return send(res, 200, { 'content-type': 'application/json' }, JSON.stringify(releaseJson()));
  }

  const routes = [
    [/^\/download\/([^/?]+)$/, m => [302, `/redirect/${m[1]}`]],
    [/^\/redirect\/([^/?]+)$/, m => [307, `relay/${m[1]}#ignored`]],
    [/^\/redirect\/relay\/([^/?]+)$/, () => [302, '?y=2']],
    [/^\/redirect\/relay\/([^/?]+)\?y=2$/, m => [302, `${base}/assets/${m[1]}`]],
    [/^\/loop\/(\d+)\/([^/?]+)$/, m => [302, `/loop/${Number(m[1]) + 1}/${m[2]}`]],
    [/^\/secure\/([^/?]+)$/, m => [302, `${base}/assets/${m[1]}`]],
  ];
  for (const [pattern, redirect] of routes) {
    const match = url.match(pattern);
    if (!match) continue;
    const [status, location] = redirect(match);
    return send(res, status, { location }, 'Redirecting');
  }
  if (/^\/assets\/ant-[a-z0-9-]+\.zip$/.test(url)) return send(res, 200, { 'content-type': 'application/zip' }, zip);
  send(res, 404, {}, 'missing');
}

const server = http.createServer(handle);
const secureServer = https.createServer({ key: tls('server-key.pem'), cert: tls('server-cert.pem') }, handle);

function runAnt(args, extraEnv = {}) {
  return new Promise((resolve, reject) => {
    const env = {
      ...process.env,
      HOME: home,
      USERPROFILE: home,
      XDG_CACHE_HOME: cache,
      ANT_MANIFEST_URL: `http://127.0.0.1:${server.address().port}/manifest`,
      ANT_GITHUB_API_URL: `http://127.0.0.1:${server.address().port}/api`,
      SSL_CERT_FILE: path.join(__dirname, 'fixtures', 'tls', 'ca-cert.pem'),
    };
    delete env.ANT_INSTALL_SOURCE;
    delete env.GITHUB_TOKEN;
    Object.assign(env, extraEnv);

    const child = spawn(ant, args, { env, stdio: ['ignore', 'pipe', 'pipe'] });
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

const paths = () => requests.map(request => request.url.replace(/ant-[a-z0-9-]+\.zip/, 'ASSET'));
const assetHops = ['/download/ASSET', '/redirect/ASSET', '/redirect/relay/ASSET', '/redirect/relay/ASSET?y=2', '/assets/ASSET'];
const manifestPaths = isCanary ? ['/manifest', '/manifest?channel=canary'] : ['/manifest'];
const github = { ANT_INSTALL_SOURCE: 'github' };
const previous = Buffer.from('previous install\n');
const upgraded = /Upgraded successfully to Ant 999\.0\.fixture\.0/;

const cases = [
  {
    name: 'github source, deflated asset with a data descriptor', env: github,
    stdout: [/Looking up stable version on GitHub releases/, upgraded, /releases\/tag\/v999\.0\.fixture\.0/],
    requests: [apiPath, ...assetHops], installed: 'payload',
    check() {
      const api = requests[0].headers;
      assert.strictEqual(api['user-agent'], `ant/${currentVersion}`);
      assert.strictEqual(api.accept, 'application/vnd.github+json');
      assert.strictEqual(api['x-github-api-version'], '2022-11-28');
      assert.strictEqual(api.authorization, undefined);
      for (const request of requests.slice(1)) assert.strictEqual(request.headers['user-agent'], `ant/${currentVersion}`);
    },
  },
  {
    name: 'GITHUB_TOKEN only reaches the API', env: { ...github, GITHUB_TOKEN: 'test-token' },
    stdout: [upgraded], installed: 'payload',
    check() {
      assert.strictEqual(requests[0].headers.authorization, 'Bearer test-token');
      for (const request of requests.slice(1)) assert.strictEqual(request.headers.authorization, undefined, request.url);
    },
  },
  { name: 'stored entry with plain local sizes', env: github, state: { zip: { method: 0, descriptor: false } }, stdout: [upgraded], installed: 'payload' },
  { name: 'deflate stream ending on an output buffer boundary', env: github, state: { payload: edgePayload }, stdout: [upgraded], installed: 'payload' },
  {
    name: 'auto falls back when the lookup fails', env: { ANT_INSTALL_SOURCE: 'auto' },
    stderr: [/falling back to GitHub releases/, ...(isCanary ? [/comparing against the latest stable release/] : [])],
    stdout: [upgraded], requests: [...manifestPaths, apiPath, ...assetHops], installed: 'payload',
  },
  {
    name: 'auto falls back when the manifest download fails', env: { ANT_INSTALL_SOURCE: 'auto' }, state: { manifest: 'up' },
    stderr: [/failed to download .*blocked-download/, /falling back to GitHub releases/],
    stdout: [upgraded], requests: ['/manifest', '/blocked-download', apiPath, ...assetHops], installed: 'payload',
  },
  {
    name: 'fallback after a failed download needs a newer GitHub release', env: { ANT_INSTALL_SOURCE: 'auto' }, status: 1,
    state: { manifest: 'up', tag: `v${currentVersion}` },
    stderr: [/failed to download .*blocked-download/, /GitHub has no release newer than/],
    requests: ['/manifest', '/blocked-download', apiPath], installed: 'unchanged',
  },
  { name: 'missing digest warns', env: github, state: { digest: 'missing' }, stderr: [/lists no digest for ant-[a-z0-9-]+\.zip, skipping checksum/], stdout: [upgraded], installed: 'payload' },
  { name: 'empty digest counts as missing', env: github, state: { digest: 'empty' }, stderr: [/lists no digest/], stdout: [upgraded], installed: 'payload' },
  {
    name: 'manifest source does not fall back', env: { ANT_INSTALL_SOURCE: 'manifest' }, status: 1,
    stderr: [/failed to download/], notStderr: /falling back/, requests: manifestPaths, installed: 'unchanged',
  },
  {
    name: 'ANT_MANIFEST_URL without a source means manifest', env: {}, status: 1,
    stderr: [/failed to download/], notStderr: /falling back/, requests: manifestPaths, installed: 'unchanged',
  },
  { name: 'tampered digest', env: github, status: 1, state: { digest: 'tampered' }, stderr: [/checksum mismatch for ant-[a-z0-9-]+\.zip: expected [0-9a-f]{64}, got [0-9a-f]{64}/], installed: 'unchanged' },
  { name: 'uppercase digest', env: github, status: 1, state: { digest: 'upper' }, stderr: [/unsupported digest/], requests: [apiPath], installed: 'unchanged' },
  { name: 'bad CRC-32', env: github, status: 1, state: { zip: { badCrc: true } }, stderr: [/CRC-32 mismatch/], installed: 'unchanged' },
  { name: 'two entries', env: github, status: 1, state: { extraEntry: true }, stderr: [/expected exactly one file in the archive, found 2/], installed: 'unchanged' },
  { name: 'empty entry', env: github, status: 1, state: { payload: Buffer.alloc(0), zip: { method: 0 } }, stderr: [/is empty/], installed: 'unchanged' },
  { name: 'download larger than the API size', env: github, status: 1, state: { shortSize: true }, stderr: [/larger than the expected size/], installed: 'unchanged' },
  {
    name: 'redirect loop', env: github, status: 1, state: { route: 'loop/0' }, stderr: [/too many redirects/], installed: 'unchanged',
    check: () => assert.strictEqual(requests.filter(request => request.url.startsWith('/loop/')).length, 6),
  },
  ...(httpsCase ? [{
    name: 'https to http redirect is refused', env: github, status: 1, state: { secure: true, route: 'secure' },
    stderr: [/refusing to follow redirect to http:/], requests: [apiPath, '/secure/ASSET'], installed: 'unchanged',
  }] : []),
  { name: 'API 403', env: github, status: 1, state: { apiStatus: 403 }, stderr: [/HTTP 403\); this may be a rate limit or a block/], installed: 'unchanged' },
  { name: 'API 401 with a token', env: { ...github, GITHUB_TOKEN: 'bad' }, status: 1, state: { apiStatus: 401 }, stderr: [/GitHub rejected GITHUB_TOKEN \(HTTP 401\)/] },
  { name: 'already up to date', env: github, state: { tag: `v${currentVersion}` }, stdout: [/already up to date/], requests: [apiPath] },
  { name: '--canary with github', env: github, args: ['--canary'], status: 1, stderr: [/canary builds are only published to manifest\.antjs\.org/], requests: [] },
  { name: 'invalid source', env: { ANT_INSTALL_SOURCE: 'elsewhere' }, status: 1, stderr: [/ANT_INSTALL_SOURCE must be auto, manifest, or github/], requests: [] },
];

async function runCase(test) {
  reset(test.state);
  fs.rmSync(bin, { recursive: true, force: true });
  if (test.installed === 'unchanged') {
    fs.mkdirSync(bin, { recursive: true });
    fs.writeFileSync(installedAnt, previous);
  }

  const result = await runAnt(['--no-color', 'upgrade', ...(test.args || [])], test.env);
  const output = `${test.name}\n${result.stdout}${result.stderr}`;
  assert.strictEqual(result.timedOut, false, output);
  assert.strictEqual(result.status, test.status || 0, output);
  for (const pattern of test.stdout || []) assert.match(result.stdout, pattern, output);
  for (const pattern of test.stderr || []) assert.match(result.stderr, pattern, output);
  if (test.notStderr) assert.doesNotMatch(result.stderr, test.notStderr, output);
  if (test.requests) assert.deepStrictEqual(paths(), test.requests, output);
  if (test.installed) {
    const expected = test.installed === 'payload' ? state.payload : previous;
    assert.ok(fs.readFileSync(installedAnt).equals(expected), `${test.name}: unexpected install contents`);
    assert.deepStrictEqual(fs.readdirSync(bin), [installedName], `${test.name}: temporary files left behind`);
  }
  if (test.check) test.check();
}

async function main() {
  for (const listener of [server, secureServer]) {
    await new Promise((resolve, reject) => {
      listener.once('error', reject);
      listener.listen(0, '127.0.0.1', resolve);
    });
  }

  try {
    for (const test of cases) await runCase(test);
    console.log('upgrade github ok');
  } finally {
    await Promise.all([server, secureServer].map(listener => new Promise(resolve => listener.close(resolve))));
    fs.rmSync(root, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
