'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { once } = require('node:events');

const { createDevApp, dev } = require('../packaging/npm/darwin-arm64/dev.cjs');

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-desktop-dev-'));
const shellQuote = value => `'${String(value).replaceAll("'", "'\\''")}'`;

async function availablePort() {
  const server = http.createServer();
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  await new Promise(resolve => server.close(resolve));
  return port;
}

async function waitForProcessExit(pid, timeout = 5_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    try {
      process.kill(pid, 0);
    } catch (error) {
      if (error.code === 'ESRCH') return;
      throw error;
    }
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  assert.fail(`process ${pid} survived renderer server shutdown`);
}

async function main() {
  const appRoot = path.join(temporary, 'source');
  const entry = path.join(appRoot, 'main.js');
  const executable = path.join(temporary, 'ant-desktop');
  const icon = path.join(temporary, 'app.icns');
  const frameworks = path.join(temporary, 'Frameworks');
  const fakeFramework = path.join(frameworks, 'Fake.framework');
  fs.mkdirSync(path.join(fakeFramework, 'Versions', 'A'), { recursive: true });
  fs.writeFileSync(path.join(fakeFramework, 'Versions', 'A', 'Fake'), 'framework');
  fs.symlinkSync('A', path.join(fakeFramework, 'Versions', 'Current'));
  fs.symlinkSync('Versions/Current/Fake', path.join(fakeFramework, 'Fake'));
  fs.mkdirSync(path.join(appRoot, 'renderer'), { recursive: true });
  fs.writeFileSync(entry, 'console.log("dev");\n');
  fs.writeFileSync(executable, 'native');
  fs.writeFileSync(icon, 'icon');

  const result = createDevApp({ executable, frameworks }, entry, {
    cacheDir: path.join(temporary, 'cache'),
    icon,
    name: 'Dev Example'
  });
  assert.equal(result.name, 'Dev Example');
  assert.ok(fs.lstatSync(result.executable).isFile());
  assert.equal(fs.readFileSync(result.executable, 'utf8'), 'native');
  const developmentFrameworks = path.join(result.output, 'Contents', 'Frameworks');
  assert.ok(fs.lstatSync(developmentFrameworks).isDirectory());
  assert.equal(fs.readlinkSync(path.join(developmentFrameworks, 'Fake.framework', 'Versions', 'Current')), 'A');
  assert.equal(fs.readFileSync(path.join(developmentFrameworks, 'Fake.framework', 'Fake'), 'utf8'), 'framework');
  const cacheSentinel = path.join(developmentFrameworks, 'cache-sentinel');
  fs.writeFileSync(cacheSentinel, 'preserved');
  createDevApp({ executable, frameworks }, entry, {
    cacheDir: path.join(temporary, 'cache'),
    icon,
    name: 'Dev Example'
  });
  assert.equal(fs.readFileSync(cacheSentinel, 'utf8'), 'preserved');

  fs.writeFileSync(path.join(frameworks, 'runtime-version'), 'updated');
  createDevApp({ executable, frameworks }, entry, {
    cacheDir: path.join(temporary, 'cache'),
    icon,
    name: 'Dev Example'
  });
  assert.equal(fs.existsSync(cacheSentinel), false);
  assert.equal(fs.realpathSync(path.join(result.output, 'Contents', 'Resources', 'app')), fs.realpathSync(appRoot));
  const plist = fs.readFileSync(path.join(result.output, 'Contents', 'Info.plist'), 'utf8');
  assert.match(plist, /<string>Dev Example<\/string>/);
  assert.match(plist, /<string>Dev Example\.icns<\/string>/);
  assert.match(plist, /<string>app\/main\.js<\/string>/);
  assert.equal(fs.realpathSync(path.join(result.output, 'Contents', 'Resources', 'Dev Example.icns')), fs.realpathSync(icon));

  fs.writeFileSync(
    path.join(appRoot, 'package.json'),
    JSON.stringify({
      productName: 'Ignored Package Name'
    })
  );
  const fallback = createDevApp({ executable, frameworks }, entry, {
    cacheDir: path.join(temporary, 'fallback-cache')
  });
  assert.equal(fallback.name, 'source');

  const exitProgram = path.join(temporary, 'exit.cjs');
  fs.writeFileSync(exitProgram, 'process.exit(0);\n');
  const supervisor = await dev({ executable: process.execPath, frameworks }, entry, {
    args: [exitProgram],
    cacheDir: path.join(temporary, 'supervisor-cache'),
    name: 'Dev Example',
    rendererBuildCommand: '/usr/bin/touch renderer-built'
  });
  await Promise.all([once(supervisor.applicationWatcher, 'close'), once(supervisor.rendererWatcher, 'close')]);
  assert.ok(fs.existsSync(path.join(appRoot, 'renderer-built')));

  const port = await availablePort();
  const rendererUrl = `http://127.0.0.1:${port}`;
  const serverProgram = path.join(temporary, 'renderer-server.cjs');
  const serverParentProgram = path.join(temporary, 'renderer-server-parent.cjs');
  const serverPidFile = path.join(temporary, 'renderer-server.pid');
  const environmentProgram = path.join(temporary, 'renderer-environment.cjs');
  fs.writeFileSync(
    serverProgram,
    `
    const fs = require('node:fs');
    const http = require('node:http');
    const server = http.createServer((_request, response) => response.end('vite'));
    server.listen(Number(process.argv[2]), '127.0.0.1', () => fs.writeFileSync(process.argv[3], String(process.pid)));
    process.once('SIGTERM', () => server.close(() => process.exit(0)));
  `
  );
  fs.writeFileSync(
    serverParentProgram,
    `
    const { spawn } = require('node:child_process');
    const child = spawn(process.execPath, process.argv.slice(2), { stdio: 'inherit' });
    child.once('exit', (code, signal) => process.exitCode = signal ? 1 : code);
  `
  );
  fs.writeFileSync(
    environmentProgram,
    `
    process.exit(process.env.ANT_DESKTOP_RENDERER_URL === process.argv[2] ? 0 : 1);
  `
  );
  const serverSupervisor = await dev({ executable: process.execPath, frameworks }, entry, {
    args: [environmentProgram, rendererUrl],
    cacheDir: path.join(temporary, 'server-supervisor-cache'),
    include: ['main.js'],
    name: 'Dev Server Example',
    rendererDevServer: {
      command: `${shellQuote(process.execPath)} ${shellQuote(serverParentProgram)} ${shellQuote(serverProgram)} ${port} ${shellQuote(serverPidFile)}`,
      url: rendererUrl
    }
  });
  assert.equal(serverSupervisor.rendererWatcher, undefined);
  await Promise.all([once(serverSupervisor.applicationWatcher, 'close'), once(serverSupervisor.rendererServer, 'exit')]);
  await waitForProcessExit(Number(fs.readFileSync(serverPidFile, 'utf8')));
  console.log('desktop-dev-bundle-ok');
}

main()
  .finally(() => {
    fs.rmSync(temporary, { recursive: true, force: true });
  })
  .catch(error => {
    console.error(error);
    process.exitCode = 1;
  });
