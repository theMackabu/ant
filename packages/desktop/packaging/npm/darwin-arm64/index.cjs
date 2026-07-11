'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const packageRoot = __dirname;
const sourceRoot = path.resolve(__dirname, '../../..');
const sourceLayout = {
  executable: path.join(sourceRoot, 'runtime', 'Ant Desktop.app', 'Contents', 'MacOS', 'Ant Desktop'),
  frameworks: path.join(sourceRoot, 'runtime', 'Ant Desktop.app', 'Contents', 'Frameworks')
};
const installedLayout = {
  executable: path.join(packageRoot, 'vendor', 'runtime', 'Ant Desktop.app', 'Contents', 'MacOS', 'Ant Desktop'),
  frameworks: path.join(packageRoot, 'vendor', 'runtime', 'Ant Desktop.app', 'Contents', 'Frameworks')
};

function isUsable(layout) {
  return fs.existsSync(layout.executable) &&
    fs.existsSync(path.join(layout.frameworks, 'Chromium Embedded Framework.framework')) &&
    fs.existsSync(path.join(layout.frameworks, 'Ant Desktop Helper.app'));
}

function ensureInstalled() {
  if (isUsable(sourceLayout)) return sourceLayout;
  if (!isUsable(installedLayout)) require('./install.cjs').install();
  if (!isUsable(installedLayout)) {
    throw new Error('The Ant Desktop native payload is incomplete; reinstall ant-desktop-darwin-arm64');
  }
  return installedLayout;
}

function assertPlatform() {
  if (process.platform !== 'darwin' || process.arch !== 'arm64') {
    throw new Error(`ant-desktop-darwin-arm64 cannot run on ${process.platform}-${process.arch}`);
  }
}

function launchOptions(options = {}) {
  assertPlatform();
  const layout = ensureInstalled();
  return {
    layout,
    options: {
      cwd: options.cwd || process.cwd(),
      env: {
        ...process.env,
        ...options.env
      },
      stdio: options.stdio || 'inherit'
    }
  };
}

function run(argv, options) {
  if (!Array.isArray(argv)) throw new TypeError('run(argv) requires an argument array');
  const launch = launchOptions(options);
  return spawn(launch.layout.executable, argv, launch.options);
}

function runSync(argv, options) {
  if (!Array.isArray(argv)) throw new TypeError('runSync(argv) requires an argument array');
  const launch = launchOptions(options);
  return spawnSync(launch.layout.executable, argv, launch.options);
}

function resolveRuntime() {
  const layout = ensureInstalled();
  return { ...layout };
}

function packageApp(entry, options) {
  assertPlatform();
  return require('./package.cjs').packageApp(ensureInstalled(), entry, options);
}

function dev(entry, options) {
  assertPlatform();
  return require('./dev.cjs').dev(ensureInstalled(), entry, options);
}

module.exports = { dev, packageApp, resolveRuntime, run, runSync };
