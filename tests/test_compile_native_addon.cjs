const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

if (process.platform === 'win32') {
  console.log('skip: compile native addon fixture does not build on Windows yet');
  process.exit(0);
}

const ant = path.resolve(process.execPath);
const buildDir = path.dirname(ant);
const repoRoot = path.dirname(buildDir);
const runtime = path.join(buildDir, 'ant-runtime');
const fixture = path.join(repoRoot, 'tests', 'fixtures', 'compile-native-addon');

if (!fs.existsSync(runtime)) {
  console.log(`skip: ${runtime} not built (ninja -C build ant-runtime)`);
  process.exit(0);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-compile-native-'));
try {
  const app = path.join(tmp, 'app');
  const nodeModules = path.join(app, 'node_modules');
  const wrapperDir = path.join(nodeModules, 'native-wrapper');
  const napiWrapperDir = path.join(nodeModules, 'napi-wrapper');
  const platformName = `native-platform-${process.platform}-${process.arch}`;
  const platformDir = path.join(nodeModules, platformName);
  const napiPlatformName = `napi-platform-${process.platform}-${process.arch}`;
  const napiPlatformDir = path.join(nodeModules, napiPlatformName);
  const napiUniversalDir = path.join(nodeModules, 'napi-platform-darwin-universal');
  const prebuildDir = path.join(platformDir, 'prebuilds', `${process.platform}-${process.arch}`);
  fs.mkdirSync(wrapperDir, { recursive: true });
  fs.mkdirSync(napiWrapperDir, { recursive: true });
  fs.mkdirSync(prebuildDir, { recursive: true });
  fs.mkdirSync(napiPlatformDir, { recursive: true });
  if (process.platform === 'darwin') fs.mkdirSync(napiUniversalDir, { recursive: true });

  fs.writeFileSync(path.join(app, 'package.json'), '{"type":"commonjs"}\n');
  fs.writeFileSync(path.join(app, 'index.cjs'), `
try { require(process.env.UNKNOWN_OPTIONAL_MODULE); } catch {}
try { require('./missing-optional.cjs'); } catch {}
const addon = require('native-wrapper');
const napiAddon = require('napi-wrapper');
console.log('native:', addon.native);
console.log('napi:', napiAddon.value);
console.log('helper:', addon.helper);
console.log('dirname:', addon.dirname);
console.log('helperPath:', addon.helperPath);
console.log('nativePath:', addon.nativePath);
`);
  fs.writeFileSync(path.join(wrapperDir, 'package.json'), '{"main":"index.cjs"}\n');
  fs.writeFileSync(path.join(wrapperDir, 'index.cjs'), `
const PACKAGE_NAME = \`native-platform-\${process.platform}-\${process.arch}\`;
function loadPlatform() {
  try { return require(PACKAGE_NAME); } catch (error) { throw error; }
}
module.exports = loadPlatform();
`);

  fs.writeFileSync(path.join(napiWrapperDir, 'package.json'), '{"main":"index.cjs"}\n');
  fs.writeFileSync(path.join(napiWrapperDir, 'index.cjs'), `
const PLATFORM_PACKAGE = \`napi-platform-\${process.platform}-\${process.arch}\`;
function loadNative() {
  if (process.env.NAPI_RS_NATIVE_LIBRARY_PATH) {
    try { return require(process.env.NAPI_RS_NATIVE_LIBRARY_PATH); } catch {}
  }
  try { return require('./binding.missing.node'); } catch {}
  if (process.platform === 'darwin') {
    try {
      const binding = require('napi-platform-darwin-universal');
      require('napi-platform-darwin-universal/package.json');
      return binding;
    } catch {}
  }
  try {
    const binding = require(PLATFORM_PACKAGE);
    require(PLATFORM_PACKAGE + '/package.json');
    return binding;
  } catch {}
  try { return require('./binding.wasi.cjs'); } catch {}
  try { return require('napi-platform-wasm32-wasi'); } catch {}
}
module.exports = loadNative();
`);

  fs.copyFileSync(path.join(fixture, 'index.cjs'), path.join(platformDir, 'index.cjs'));
  fs.writeFileSync(path.join(platformDir, 'package.json'), '{"main":"index.cjs"}\n');
  const helperPath = path.join(prebuildDir, 'helper');
  fs.copyFileSync(path.join(fixture, 'helper'), helperPath);
  fs.chmodSync(helperPath, 0o755);

  const addonPath = path.join(prebuildDir, 'binding.node');
  const ccArgs = process.platform === 'darwin'
    ? ['-bundle', '-undefined', 'dynamic_lookup', path.join(fixture, 'binding.c'), '-o', addonPath]
    : ['-shared', '-fPIC', path.join(fixture, 'binding.c'), '-o', addonPath];
  let result = spawnSync(process.env.CC || 'cc', ccArgs, { encoding: 'utf8' });
  assert.equal(result.status, 0, `native fixture build failed: ${result.stderr}`);
  fs.copyFileSync(addonPath, path.join(napiPlatformDir, 'binding.node'));
  fs.writeFileSync(path.join(napiPlatformDir, 'package.json'), '{"main":"binding.node"}\n');
  if (process.platform === 'darwin') {
    const universalPath = path.join(napiUniversalDir, 'binding.node');
    const universalArgs = [...ccArgs];
    universalArgs.splice(universalArgs.length - 2, 0, '-DNATIVE_VALUE="universal-duplicate-marker"');
    result = spawnSync(process.env.CC || 'cc', universalArgs.slice(0, -1).concat(universalPath), { encoding: 'utf8' });
    assert.equal(result.status, 0, `universal native fixture build failed: ${result.stderr}`);
    fs.writeFileSync(path.join(napiUniversalDir, 'package.json'),
      '{"main":"binding.node","universalMarker":"universal-package-json-marker"}\n');
  }

  const out = path.join(tmp, 'compiled-app');
  result = spawnSync(ant, ['compile', '--runtime', runtime, '-o', out, path.join(app, 'index.cjs')], {
    encoding: 'utf8',
  });
  assert.equal(result.status, 0, `compile failed: ${result.stderr}`);
  assert.match(result.stderr, /non-constant specifier is not traced inside try\/catch/);
  assert.equal((result.stderr.match(/non-constant specifier is not traced/g) || []).length, 1, result.stderr);
  assert.match(result.stderr, /optional module "\.\/missing-optional\.cjs" was not found/);
  assert.doesNotMatch(result.stderr, /binding\.missing\.node|binding\.wasi\.cjs|napi-platform-wasm32-wasi|NAPI_RS_NATIVE_LIBRARY_PATH/);
  if (process.platform === 'darwin') {
    assert.ok(!fs.readFileSync(out).includes(Buffer.from('universal-duplicate-marker')), 'universal addon was duplicated in bundle');
    assert.ok(!fs.readFileSync(out).includes(Buffer.from('universal-package-json-marker')), 'universal package metadata was duplicated in bundle');
  }

  result = spawnSync(out, [], { encoding: 'utf8', cwd: os.homedir() });
  assert.equal(result.status, 0, `compiled native app failed: ${result.stderr}`);
  assert.match(result.stdout, /native: native-ok/);
  assert.match(result.stdout, /napi: native-ok/);
  assert.match(result.stdout, /helper: helper-ok/);

  const dirname = result.stdout.match(/^dirname: (.+)$/m)?.[1];
  const extractedHelper = result.stdout.match(/^helperPath: (.+)$/m)?.[1];
  const extractedNative = result.stdout.match(/^nativePath: (.+)$/m)?.[1];
  assert.ok(dirname && path.isAbsolute(dirname), result.stdout);
  assert.ok(!dirname.startsWith('/$ant/'), result.stdout);
  assert.ok(extractedHelper && !fs.existsSync(extractedHelper), 'native asset temp tree was not removed');
  assert.ok(extractedNative && path.isAbsolute(extractedNative), result.stdout);
  assert.ok(!fs.existsSync(extractedNative), 'native addon temp file was not removed');
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}
