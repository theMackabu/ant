import fs from 'node:fs';
import path from 'node:path';

import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { spawn, spawnSync } from 'node:child_process';

const require = createRequire(import.meta.url);
const packageRoot = path.dirname(fileURLToPath(import.meta.url));

const platforms = require('./platforms.json');
const supportedPlatforms = new Set(platforms.supported);

export function platformKey() {
  const platform = platforms.names[process.platform];
  const key = platform && `${platform}-${process.arch}`;

  if (!key || !supportedPlatforms.has(key))
    throw new Error(`Ant Desktop does not currently provide a binary for ${key || `${process.platform}-${process.arch}`}`);

  return key;
}

export function platformPackageName() {
  return `ant-desktop-${platformKey()}`;
}

const drivers = { darwin: './platform/darwin.cjs', linux: './platform/directory.cjs', win32: './platform/directory.cjs' };

function loadPayload() {
  const configured = process.env.ANT_DESKTOP_PLATFORM_PACKAGE;
  if (configured) return require(path.resolve(configured));
  const name = platformPackageName();
  const sourcePackage = path.join(packageRoot, 'packaging', 'npm', platformKey());
  if (fs.existsSync(path.join(sourcePackage, 'package.json'))) return require(sourcePackage);

  try {
    return require(name);
  } catch (error) {
    if (error && error.code !== 'MODULE_NOT_FOUND') throw error;
    throw new Error(`The optional platform package ${name} is missing. Reinstall ant-desktop without --omit=optional.`, { cause: error });
  }
}

function loadPlatform() {
  const payload = loadPayload();
  const driver = require(drivers[process.platform]);
  const layout = [path.join(packageRoot, 'runtime'), path.join(payload.root, 'vendor')]
    .map(root => driver.layout(root))
    .find(value => fs.existsSync(value.executable) && fs.existsSync(value.library));
  if (!layout) throw new Error(`The Ant Desktop native payload is missing; reinstall ${platformPackageName()}`);
  return { driver, layout: { ...layout, lock: payload.lock } };
}

function launchOptions(options = {}) {
  return {
    cwd: options.cwd || process.cwd(),
    env: { ...process.env, ...options.env },
    stdio: options.stdio || 'inherit'
  };
}

export function run(entry, args = [], options) {
  if (typeof entry !== 'string' || !entry) throw new TypeError('run(entry) requires an application entry file');
  return spawn(loadPlatform().layout.executable, [entry, ...args], launchOptions(options));
}

export function runSync(entry, args = [], options) {
  if (typeof entry !== 'string' || !entry) throw new TypeError('runSync(entry) requires an application entry file');
  return spawnSync(loadPlatform().layout.executable, [entry, ...args], launchOptions(options));
}

export function resolveRuntime() {
  return loadPlatform().layout;
}

export function packageApp(entry, options) {
  if (typeof entry !== 'string' || !entry) {
    throw new TypeError('packageApp(entry) requires an application entry file');
  }
  const { driver, layout } = loadPlatform();
  return driver.packageApp(layout, entry, options);
}

export function dev(entry, options) {
  if (typeof entry !== 'string' || !entry) {
    throw new TypeError('dev(entry) requires an application entry file');
  }
  const { driver, layout } = loadPlatform();
  return require('./platform/common.cjs').dev(driver, layout, entry, options);
}
