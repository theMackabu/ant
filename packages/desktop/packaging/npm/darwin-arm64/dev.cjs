'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const os = require('node:os');
const path = require('node:path');
const { execFileSync, spawn } = require('node:child_process');
const { buildRenderer } = require('./renderer.cjs');

const { applicationName, bundleIdentifier, infoPlist, isInside, matchesInclude } = require('./package.cjs');

function replaceSymlink(target, link, type) {
  fs.rmSync(link, { recursive: true, force: true });
  fs.symlinkSync(target, link, type);
}

function cloneFile(source, destination) {
  fs.rmSync(destination, { recursive: true, force: true });
  fs.copyFileSync(source, destination, fs.constants.COPYFILE_FICLONE);
  fs.chmodSync(destination, fs.statSync(source).mode);
}

function cloneTree(source, destination) {
  const stat = fs.lstatSync(source);
  if (stat.isSymbolicLink()) {
    fs.symlinkSync(fs.readlinkSync(source), destination);
    return;
  }
  if (stat.isDirectory()) {
    fs.mkdirSync(destination, { recursive: true, mode: stat.mode });
    for (const entry of fs.readdirSync(source)) {
      cloneTree(path.join(source, entry), path.join(destination, entry));
    }
    return;
  }
  if (!stat.isFile()) throw new Error(`Unsupported Chromium runtime entry: ${source}`);
  fs.copyFileSync(source, destination, fs.constants.COPYFILE_FICLONE);
  fs.chmodSync(destination, stat.mode);
}

function runtimeFingerprint(frameworks) {
  const entries = [frameworks, ...fs.readdirSync(frameworks).map(entry => path.join(frameworks, entry))];
  const hash = crypto.createHash('sha256');
  for (const entry of entries) {
    const stat = fs.lstatSync(entry);
    hash.update(`${path.resolve(entry)}\0${stat.size}\0${stat.mtimeMs}\0${stat.mode}\0`);
  }
  return hash.digest('hex');
}

function ensureRuntimeFrameworks(source, contents) {
  const destination = path.join(contents, 'Frameworks');
  const marker = path.join(contents, '.ant-runtime.json');
  const fingerprint = runtimeFingerprint(source);
  try {
    const cached = JSON.parse(fs.readFileSync(marker, 'utf8'));
    if (cached.fingerprint === fingerprint && fs.lstatSync(destination).isDirectory()) return;
  } catch {
    // A missing or incomplete cache is rebuilt below.
  }

  const temporary = `${destination}.tmp-${process.pid}`;
  fs.rmSync(temporary, { recursive: true, force: true });
  try {
    cloneTree(source, temporary);
    fs.rmSync(destination, { recursive: true, force: true });
    fs.renameSync(temporary, destination);
    fs.writeFileSync(marker, `${JSON.stringify({ fingerprint })}\n`);
  } catch (error) {
    fs.rmSync(temporary, { recursive: true, force: true });
    throw error;
  }
}

function createDevApp(layout, entry, options = {}) {
  const sourceEntry = path.resolve(entry);
  if (!fs.statSync(sourceEntry, { throwIfNoEntry: false })?.isFile()) {
    throw new Error(`Application entry does not exist: ${sourceEntry}`);
  }
  const appRoot = path.resolve(options.appDir || path.dirname(sourceEntry));
  if (!isInside(appRoot, sourceEntry)) {
    throw new Error('Application entry must be inside --app-dir');
  }

  const name = applicationName(options.name || path.basename(appRoot));
  const identifier = bundleIdentifier(name, options.identifier);
  const key = crypto.createHash('sha256').update(`${appRoot}\0${name}\0${identifier}`).digest('hex').slice(0, 12);
  const output = path.join(options.cacheDir || path.join(os.homedir(), 'Library', 'Caches', 'ant-desktop'), 'dev', key, `${name}.app`);
  const contents = path.join(output, 'Contents');
  const macos = path.join(contents, 'MacOS');
  const resources = path.join(contents, 'Resources');
  const executable = path.join(macos, name);
  let icon;

  if (options.icon) {
    const sourceIcon = path.resolve(options.icon);
    if (path.extname(sourceIcon).toLowerCase() !== '.icns' || !fs.statSync(sourceIcon, { throwIfNoEntry: false })?.isFile()) {
      throw new Error('macOS application icon must be an existing .icns file');
    }
    icon = `${name}.icns`;
  }

  fs.mkdirSync(macos, { recursive: true });
  fs.mkdirSync(resources, { recursive: true });
  cloneFile(layout.executable, executable);
  ensureRuntimeFrameworks(layout.frameworks, contents);
  replaceSymlink(appRoot, path.join(resources, 'app'), 'dir');
  if (options.icon) {
    replaceSymlink(path.resolve(options.icon), path.join(resources, icon), 'file');
  }
  fs.writeFileSync(
    path.join(contents, 'Info.plist'),
    infoPlist({
      name,
      executable: name,
      identifier,
      version: options.version || '0.0.0-dev',
      icon,
      entry: path.posix.join('app', path.relative(appRoot, sourceEntry).split(path.sep).join('/'))
    })
  );
  fs.writeFileSync(path.join(contents, 'PkgInfo'), 'APPL????');
  return { appRoot, executable, name, output };
}

function delay(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

function terminateProcessTree(child) {
  if (!child?.pid) return;
  const children = new Map();
  try {
    const processes = execFileSync('/bin/ps', ['-axo', 'pid=,ppid='], { encoding: 'utf8' });
    for (const line of processes.trim().split('\n')) {
      const [pid, parentPid] = line.trim().split(/\s+/).map(Number);
      if (!children.has(parentPid)) children.set(parentPid, []);
      children.get(parentPid).push(pid);
    }
  } catch {
    // the direct child is still safe to terminate if process inspection fails
  }

  const pids = [];
  const collect = pid => {
    for (const descendant of children.get(pid) || []) collect(descendant);
    pids.push(pid);
  };
  collect(child.pid);
  for (const pid of pids) {
    try {
      process.kill(pid, 'SIGTERM');
    } catch (error) {
      if (error.code !== 'ESRCH') throw error;
    }
  }
}

function devServerReady(value) {
  return new Promise(resolve => {
    let url;
    try {
      url = new URL(value);
    } catch {
      resolve(false);
      return;
    }
    const client = url.protocol === 'https:' ? https : http;
    const request = client.get(url, response => {
      response.resume();
      resolve(true);
    });
    request.setTimeout(500, () => request.destroy());
    request.once('error', () => resolve(false));
  });
}

async function waitForDevServer(process, url, timeout = 30_000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (process.exitCode !== null || process.signalCode) {
      throw new Error(`Renderer dev server exited before ${url} became available`);
    }
    if (await devServerReady(url)) return;
    await delay(100);
  }
  throw new Error(`Timed out waiting for renderer dev server at ${url}`);
}

async function dev(layout, entry, options = {}) {
  const developmentApp = createDevApp(layout, entry, options);
  const devServerOptions = options.rendererDevServer;
  const rendererRoot = devServerOptions ? undefined : path.resolve(options.rendererDir || path.join(developmentApp.appRoot, 'renderer'));
  if (rendererRoot && !fs.statSync(rendererRoot, { throwIfNoEntry: false })?.isDirectory()) {
    throw new Error(`Renderer directory does not exist: ${rendererRoot} (set renderer.watchDir or renderer.devServer)`);
  }
  if (!devServerOptions) buildRenderer(options.rendererBuildCommand, developmentApp.appRoot);
  let child;
  let rendererServer;
  let restartTimer;
  let rendererBuildTimer;
  let applicationWatcher;
  let rendererWatcher;
  let restarting = false;
  let stopping = false;
  let rendererChangeAt = 0;
  let ignoreRendererChangesUntil = 0;

  const launch = () => {
    child = spawn(developmentApp.executable, options.args || [], {
      cwd: developmentApp.appRoot,
      env: {
        ...process.env,
        ...options.env,
        ANT_DESKTOP_DEV: '1',
        ...(devServerOptions ? { ANT_DESKTOP_RENDERER_URL: devServerOptions.url } : {})
      },
      stdio: 'inherit'
    });
    child.once('exit', (code, signal) => {
      if (stopping || restarting || restartTimer) return;
      if (code !== 0) {
        process.stderr.write(`ant-desktop dev: application exited (${signal || code})\n`);
      }
      stop();
    });
  };

  const restart = () => {
    clearTimeout(restartTimer);
    restartTimer = setTimeout(() => {
      restartTimer = undefined;
      if (!child || child.exitCode !== null) {
        launch();
        return;
      }
      restarting = true;
      child.once('exit', () => {
        restarting = false;
        launch();
      });
      child.kill('SIGTERM');
    }, 100);
  };

  if (rendererRoot) {
    rendererWatcher = fs.watch(rendererRoot, { recursive: true }, () => {
      if (Date.now() < ignoreRendererChangesUntil) return;
      rendererChangeAt = Date.now();
      clearTimeout(rendererBuildTimer);
      rendererBuildTimer = setTimeout(() => {
        rendererBuildTimer = undefined;
        try {
          buildRenderer(options.rendererBuildCommand, developmentApp.appRoot);
        } catch (error) {
          process.stderr.write(`ant-desktop dev: ${error.message}\n`);
          return;
        }
        rendererChangeAt = Date.now();
        ignoreRendererChangesUntil = rendererChangeAt + 250;
        if (child?.exitCode === null) child.kill('SIGUSR1');
      }, 100);
    });
  }
  applicationWatcher = fs.watch(developmentApp.appRoot, { recursive: true }, (_event, filename) => {
    if (stopping) return;
    if (!filename) return;
    const changed = path.resolve(developmentApp.appRoot, filename);
    if (filename.split(path.sep).some(part => part === '.git' || part === 'node_modules')) return;
    if (rendererRoot && (isInside(rendererRoot, changed) || Date.now() - rendererChangeAt < 250)) {
      return;
    }
    const applicationEntry = path.resolve(entry);
    if (changed !== applicationEntry && !matchesInclude(filename, options.include || [])) return;
    restart();
  });

  const stop = () => {
    if (stopping) return;
    stopping = true;
    clearTimeout(restartTimer);
    clearTimeout(rendererBuildTimer);
    rendererWatcher?.close();
    applicationWatcher?.close();
    if (child?.exitCode === null) child.kill('SIGTERM');
    terminateProcessTree(rendererServer);
  };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
  if (devServerOptions) {
    rendererServer = spawn(devServerOptions.command, [], {
      cwd: developmentApp.appRoot,
      env: { ...process.env, ...options.env },
      shell: true,
      stdio: 'inherit'
    });
    try {
      await waitForDevServer(rendererServer, devServerOptions.url);
    } catch (error) {
      stop();
      throw error;
    }
    rendererServer.once('exit', (code, signal) => {
      if (stopping) return;
      process.stderr.write(`ant-desktop dev: renderer dev server exited (${signal || code})\n`);
      stop();
    });
  }
  launch();
  process.stdout.write(`Ant Desktop dev app: ${developmentApp.output}\n`);
  return {
    ...developmentApp,
    applicationWatcher,
    rendererWatcher,
    rendererServer,
    stop
  };
}

module.exports = { createDevApp, dev, devServerReady, waitForDevServer };
