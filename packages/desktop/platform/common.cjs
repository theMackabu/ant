'use strict';

const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const path = require('node:path');
const { execFileSync, spawn, spawnSync } = require('node:child_process');

function buildRenderer(command, cwd) {
  if (!command) return;
  const result = spawnSync(command, { cwd, env: process.env, shell: true, stdio: 'inherit' });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`Renderer build failed (${result.signal || result.status})`);
}

function applicationName(value) {
  if (!value.trim() || /[/:\\]/.test(value) || value.includes('\0')) {
    throw new Error('Application name must not be empty or contain /, :, or NUL');
  }
  return value.trim();
}

function bundleIdentifier(name, configured) {
  const fallback = name.toLowerCase().replace(/[^a-z0-9]+/g, '-');
  const value = configured || `org.antjs.${fallback}`;
  if (!/^[A-Za-z0-9.-]+$/.test(value)) {
    throw new Error('Application identifier may contain only letters, numbers, dots, and hyphens');
  }
  return value;
}

function isInside(parent, child) {
  const relative = path.relative(parent, child);
  return relative === '' || (!relative.startsWith('..') && !path.isAbsolute(relative));
}

function copyTree(source, destination, excluded, include = () => true) {
  fs.cpSync(source, destination, {
    recursive: true,
    dereference: false,
    verbatimSymlinks: true,
    filter(candidate) {
      const resolved = path.resolve(candidate);
      return include(candidate) && !excluded.some(value => isInside(value, resolved));
    }
  });
}

function includeExpression(value) {
  if (typeof value !== 'string' || !value.trim() || path.isAbsolute(value)) {
    throw new Error(`Include pattern must be a relative glob: ${value}`);
  }
  const normalized = value.replaceAll('\\', '/');
  if (normalized.split('/').some(part => !part || part === '.' || part === '..')) {
    throw new Error(`Include pattern must stay inside the application directory: ${value}`);
  }
  let expression = '^';
  for (let index = 0; index < normalized.length; index++) {
    const character = normalized[index];
    if (character === '*' && normalized[index + 1] === '*') {
      index++;
      if (normalized[index + 1] === '/') {
        index++;
        expression += '(?:.*/)?';
      } else {
        expression += '.*';
      }
    } else if (character === '*') {
      expression += '[^/]*';
    } else if (character === '?') {
      expression += '[^/]';
    } else {
      expression += character.replace(/[|\\{}()[\]^$+?.]/g, '\\$&');
    }
  }
  return { pattern: value, expression: new RegExp(`${expression}$`) };
}

function matchesInclude(relative, patterns) {
  const normalized = relative.split(path.sep).join('/');
  return patterns.some(value => includeExpression(value).expression.test(normalized));
}

function includedSources(root, values, excluded = []) {
  if (!Array.isArray(values)) throw new TypeError('include must be an array');
  const patterns = values.map(includeExpression);
  const matches = new Array(patterns.length).fill(0);
  const included = [];
  const visit = current => {
    const resolved = path.resolve(current);
    if (excluded.some(value => isInside(value, resolved))) return;
    const stat = fs.lstatSync(current);
    if (stat.isDirectory()) {
      for (const name of fs.readdirSync(current).sort()) visit(path.join(current, name));
      return;
    }
    const destination = path.relative(root, current).split(path.sep).join('/');
    let selected = false;
    patterns.forEach((pattern, index) => {
      if (!pattern.expression.test(destination)) return;
      matches[index]++;
      selected = true;
    });
    if (selected) included.push({ source: current, destination, stat });
  };
  visit(root);
  const missing = patterns.find((_pattern, index) => matches[index] === 0);
  if (missing) throw new Error(`Include pattern matched no files: ${missing.pattern}`);
  return included;
}

function copyIncluded(included, destination) {
  for (const entry of included) {
    const target = path.join(destination, entry.destination);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    if (entry.stat.isSymbolicLink()) fs.symlinkSync(fs.readlinkSync(entry.source), target);
    else fs.copyFileSync(entry.source, target);
  }
}

function bundledEntryPath(included, sourceEntry) {
  const entry = included.find(value => path.resolve(value.source) === sourceEntry);
  if (!entry) {
    throw new Error('Application entry is not included');
  }
  return entry.destination;
}

function resolveApplication(entry, options) {
  const sourceEntry = path.resolve(entry);
  if (!fs.statSync(sourceEntry, { throwIfNoEntry: false })?.isFile()) {
    throw new Error(`Application entry does not exist: ${sourceEntry}`);
  }
  const appRoot = path.resolve(options.appDir || path.dirname(sourceEntry));
  if (!fs.statSync(appRoot, { throwIfNoEntry: false })?.isDirectory()) {
    throw new Error(`Application directory does not exist: ${appRoot}`);
  }
  if (!isInside(appRoot, sourceEntry)) {
    throw new Error('Application entry must be inside --app-dir');
  }
  return { appRoot, sourceEntry };
}

function applicationIcon(file, extension, label) {
  if (!file) return undefined;
  const source = path.resolve(file);
  if (path.extname(source).toLowerCase() !== extension || !fs.statSync(source, { throwIfNoEntry: false })?.isFile()) {
    throw new Error(`${label} application icon must be an existing ${extension} file`);
  }
  return source;
}

function sharedRuntime(directory) {
  if (!fs.existsSync(directory)) {
    throw new Error(`The shared browser runtime is not installed (${directory}); run an Ant Desktop app once to fetch it`);
  }
  return directory;
}

function replaceOutput(temporary, output, build) {
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.rmSync(temporary, { recursive: true, force: true });
  try {
    build();
    if (fs.existsSync(output)) fs.rmSync(output, { recursive: true, force: true });
    fs.renameSync(temporary, output);
  } catch (error) {
    fs.rmSync(temporary, { recursive: true, force: true });
    throw error;
  }
}

function replaceSymlink(target, link, type) {
  fs.rmSync(link, { recursive: true, force: true });
  fs.symlinkSync(target, link, type);
}

function cloneFile(source, destination) {
  fs.rmSync(destination, { recursive: true, force: true });
  fs.copyFileSync(source, destination, fs.constants.COPYFILE_FICLONE);
  fs.chmodSync(destination, fs.statSync(source).mode);
}

function delay(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds));
}

function reload(child, developmentApp) {
  if (process.platform !== 'win32') child.kill('SIGUSR1');
  else spawnSync(developmentApp.executable, [`--reload=${child.pid}`], { stdio: 'ignore' });
}

function terminateProcessTree(child) {
  if (!child?.pid) return;
  if (process.platform === 'win32') {
    spawnSync('taskkill', ['/pid', String(child.pid), '/t', '/f'], { stdio: 'ignore' });
    return;
  }
  const children = new Map();
  try {
    const processes = execFileSync('ps', ['-axo', 'pid=,ppid='], { encoding: 'utf8' });
    for (const line of processes.trim().split('\n')) {
      const [pid, parentPid] = line.trim().split(/\s+/).map(Number);
      if (!children.has(parentPid)) children.set(parentPid, []);
      children.get(parentPid).push(pid);
    }
  } catch {}

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

async function dev(driver, layout, entry, options = {}) {
  const developmentApp = driver.createDevApp(layout, entry, options);
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
        if (child?.exitCode === null) reload(child, developmentApp);
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

module.exports = {
  applicationIcon,
  applicationName,
  buildRenderer,
  bundledEntryPath,
  bundleIdentifier,
  cloneFile,
  copyIncluded,
  copyTree,
  dev,
  devServerReady,
  includedSources,
  isInside,
  matchesInclude,
  replaceOutput,
  replaceSymlink,
  resolveApplication,
  sharedRuntime,
  waitForDevServer
};
