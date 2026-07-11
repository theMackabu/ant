'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');

const { applicationName, bundleIdentifier, infoPlist, isInside, resourceDestination } = require('./package.cjs');

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

function runtimeFingerprint(hostBundle, host) {
  const frameworks = path.join(hostBundle, 'Contents', 'Frameworks');
  const entries = [host, frameworks, ...fs.readdirSync(frameworks).map(entry => path.join(frameworks, entry))];
  const hash = crypto.createHash('sha256');
  for (const entry of entries) {
    const stat = fs.lstatSync(entry);
    hash.update(`${path.resolve(entry)}\0${stat.size}\0${stat.mtimeMs}\0${stat.mode}\0`);
  }
  return hash.digest('hex');
}

function ensureRuntimeFrameworks(hostBundle, contents) {
  const source = path.join(hostBundle, 'Contents', 'Frameworks');
  const destination = path.join(contents, 'Frameworks');
  const marker = path.join(contents, '.ant-runtime.json');
  const fingerprint = runtimeFingerprint(hostBundle, path.join(hostBundle, 'Contents', 'MacOS', 'Ant Chromium Host'));
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
  const hostBundle = path.resolve(layout.host, '../../..');
  let icon;

  if (options.icon) {
    const sourceIcon = path.resolve(options.icon);
    if (path.extname(sourceIcon).toLowerCase() !== '.icns' || !fs.statSync(sourceIcon, { throwIfNoEntry: false })?.isFile()) {
      throw new Error('macOS application icon must be an existing .icns file');
    }
    icon = `${name}.icns`;
  }
  const extraResources = (options.extraResources || []).map(value =>
    resourceDestination(resources, typeof value === 'string' ? { from: value } : value)
  );

  fs.mkdirSync(macos, { recursive: true });
  fs.mkdirSync(resources, { recursive: true });
  cloneFile(layout.executable, executable);
  cloneFile(layout.host, path.join(macos, 'Ant Chromium Host'));
  ensureRuntimeFrameworks(hostBundle, contents);
  replaceSymlink(appRoot, path.join(resources, 'app'), 'dir');
  for (const resource of extraResources) {
    fs.mkdirSync(path.dirname(resource.destination), { recursive: true });
    replaceSymlink(resource.source, resource.destination, fs.statSync(resource.source).isDirectory() ? 'dir' : 'file');
  }
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

function dev(layout, entry, options = {}) {
  const developmentApp = createDevApp(layout, entry, options);
  const rendererRoot = path.resolve(options.rendererDir || path.join(developmentApp.appRoot, 'renderer'));
  if (!fs.statSync(rendererRoot, { throwIfNoEntry: false })?.isDirectory()) {
    throw new Error(`Renderer directory does not exist: ${rendererRoot} (pass --renderer-dir)`);
  }
  let child;
  let restartTimer;
  let restarting = false;
  let stopping = false;
  let rendererChangeAt = 0;

  const launch = () => {
    child = spawn(developmentApp.executable, options.args || [], {
      cwd: developmentApp.appRoot,
      env: { ...process.env, ...options.env, ANT_DESKTOP_DEV: '1' },
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

  const rendererWatcher = fs.watch(rendererRoot, { recursive: true }, () => {
    rendererChangeAt = Date.now();
    if (child?.exitCode === null) child.kill('SIGUSR1');
  });
  const applicationWatcher = fs.watch(developmentApp.appRoot, { recursive: true }, (_event, filename) => {
    if (stopping) return;
    if (!filename) return;
    const changed = path.resolve(developmentApp.appRoot, filename);
    if (filename.split(path.sep).some(part => part === '.git' || part === 'node_modules' || part === 'dist')) return;
    if (isInside(rendererRoot, changed) || Date.now() - rendererChangeAt < 250) {
      return;
    }
    restart();
  });

  const stop = () => {
    if (stopping) return;
    stopping = true;
    clearTimeout(restartTimer);
    rendererWatcher.close();
    applicationWatcher.close();
    if (child?.exitCode === null) child.kill('SIGTERM');
  };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
  launch();
  process.stdout.write(`Ant Desktop dev app: ${developmentApp.output}\n`);
  return {
    ...developmentApp,
    applicationWatcher,
    rendererWatcher,
    stop
  };
}

module.exports = { createDevApp, dev };
