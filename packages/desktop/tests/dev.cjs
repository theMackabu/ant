'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');

const {
  createDevApp,
  dev
} = require('../packaging/npm/darwin-arm64/dev.cjs');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-desktop-dev-'));
async function main() {
  const appRoot = path.join(temporary, 'source');
  const entry = path.join(appRoot, 'main.js');
  const executable = path.join(temporary, 'ant-desktop');
  const icon = path.join(temporary, 'app.icns');
  const hostBundle = path.join(temporary, 'Ant Chromium Host.app');
  const host = path.join(hostBundle, 'Contents', 'MacOS', 'Ant Chromium Host');
  fs.mkdirSync(path.dirname(host), { recursive: true });
  fs.mkdirSync(path.join(hostBundle, 'Contents', 'Frameworks'), {
    recursive: true
  });
  const fakeFramework = path.join(hostBundle, 'Contents', 'Frameworks', 'Fake.framework');
  fs.mkdirSync(path.join(fakeFramework, 'Versions', 'A'), { recursive: true });
  fs.writeFileSync(path.join(fakeFramework, 'Versions', 'A', 'Fake'), 'framework');
  fs.symlinkSync('A', path.join(fakeFramework, 'Versions', 'Current'));
  fs.symlinkSync('Versions/Current/Fake', path.join(fakeFramework, 'Fake'));
  fs.mkdirSync(path.join(appRoot, 'renderer'), { recursive: true });
  fs.writeFileSync(entry, 'console.log("dev");\n');
  fs.writeFileSync(executable, 'native');
  fs.writeFileSync(icon, 'icon');
  fs.writeFileSync(host, 'chromium');

  const result = createDevApp({ executable, host }, entry, {
    cacheDir: path.join(temporary, 'cache'),
    icon,
    name: 'Dev Example'
  });
  assert.equal(result.name, 'Dev Example');
  assert.ok(fs.lstatSync(result.executable).isFile());
  assert.equal(fs.readFileSync(result.executable, 'utf8'), 'native');
  const developmentFrameworks = path.join(result.output, 'Contents', 'Frameworks');
  assert.ok(fs.lstatSync(developmentFrameworks).isDirectory());
  assert.equal(
    fs.readlinkSync(path.join(developmentFrameworks, 'Fake.framework', 'Versions', 'Current')),
    'A'
  );
  assert.equal(
    fs.readFileSync(path.join(developmentFrameworks, 'Fake.framework', 'Fake'), 'utf8'),
    'framework'
  );
  const cacheSentinel = path.join(developmentFrameworks, 'cache-sentinel');
  fs.writeFileSync(cacheSentinel, 'preserved');
  createDevApp({ executable, host }, entry, {
    cacheDir: path.join(temporary, 'cache'),
    icon,
    name: 'Dev Example'
  });
  assert.equal(fs.readFileSync(cacheSentinel, 'utf8'), 'preserved');

  fs.appendFileSync(host, '-updated');
  createDevApp({ executable, host }, entry, {
    cacheDir: path.join(temporary, 'cache'),
    icon,
    name: 'Dev Example'
  });
  assert.equal(fs.existsSync(cacheSentinel), false);
  assert.equal(
    fs.realpathSync(path.join(result.output, 'Contents', 'Resources', 'app')),
    fs.realpathSync(appRoot)
  );
  const plist = fs.readFileSync(
    path.join(result.output, 'Contents', 'Info.plist'),
    'utf8'
  );
  assert.match(plist, /<string>Dev Example<\/string>/);
  assert.match(plist, /<string>Dev Example\.icns<\/string>/);
  assert.match(plist, /<string>app\/main\.js<\/string>/);
  assert.equal(
    fs.realpathSync(path.join(result.output, 'Contents', 'Resources', 'Dev Example.icns')),
    fs.realpathSync(icon)
  );

  fs.writeFileSync(path.join(appRoot, 'package.json'), JSON.stringify({
    productName: 'Ignored Package Name'
  }));
  const fallback = createDevApp({ executable, host }, entry, {
    cacheDir: path.join(temporary, 'fallback-cache')
  });
  assert.equal(fallback.name, 'source');

  const exitProgram = path.join(temporary, 'exit.cjs');
  fs.writeFileSync(exitProgram, 'process.exit(0);\n');
  const supervisor = dev({ executable: process.execPath, host }, entry, {
    args: [exitProgram],
    cacheDir: path.join(temporary, 'supervisor-cache'),
    name: 'Dev Example'
  });
  await Promise.all([
    once(supervisor.applicationWatcher, 'close'),
    once(supervisor.rendererWatcher, 'close')
  ]);
  console.log('desktop-dev-bundle-ok');
}

main().finally(() => {
  fs.rmSync(temporary, { recursive: true, force: true });
}).catch(error => {
  console.error(error);
  process.exitCode = 1;
});
