'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');

const layouts = [path.resolve(__dirname, '../../../runtime'), path.join(__dirname, 'vendor')].map(root => ({
  executable: path.join(root, 'Ant Desktop.app', 'Contents', 'MacOS', 'Ant Desktop'),
  frameworks: path.join(root, 'Ant Desktop.app', 'Contents', 'Frameworks')
}));

function ensureInstalled() {
  const layout = layouts.find(value => fs.existsSync(value.executable) && fs.existsSync(path.join(value.frameworks, 'libantdesktop.dylib')));
  if (!layout) throw new Error('The Ant Desktop native payload is missing; reinstall ant-desktop-darwin-arm64');
  return layout;
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
