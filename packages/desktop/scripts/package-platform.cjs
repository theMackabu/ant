#!/usr/bin/env node
'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const { execute } = require('./lib/command.cjs');

const desktopRoot = path.resolve(__dirname, '..');
const packageRoot = path.join(desktopRoot, 'packaging', 'npm', 'darwin-arm64');

function sha256(filename) {
  const hash = crypto.createHash('sha256');
  hash.update(fs.readFileSync(filename));
  return hash.digest('hex');
}

function packagePlatform() {
  const executable = path.join(desktopRoot, 'build', 'ant-desktop');
  const app = path.join(desktopRoot, 'runtime', 'Ant Desktop.app');
  const bundledExecutable = path.join(app, 'Contents', 'MacOS', 'Ant Desktop');
  const framework = path.join(app, 'Contents', 'Frameworks', 'Chromium Embedded Framework.framework');
  if (!fs.existsSync(framework)) {
    execute(process.execPath, [path.join(__dirname, 'build-browser-host.cjs')]);
  }
  if (!fs.existsSync(executable)) {
    execute(process.execPath, [path.join(__dirname, 'build-app.cjs')]);
  } else {
    fs.copyFileSync(executable, bundledExecutable);
    fs.chmodSync(bundledExecutable, 0o755);
  }

  const artifacts = path.join(packageRoot, 'artifacts');
  const archive = path.join(artifacts, 'ant-desktop-darwin-arm64.tar.gz');
  fs.mkdirSync(artifacts, { recursive: true });
  execute('tar', ['-czf', archive, 'runtime/Ant Desktop.app'], {
    cwd: desktopRoot,
    env: { ...process.env, COPYFILE_DISABLE: '1' }
  });
  fs.writeFileSync(`${archive}.sha256`, `${sha256(archive)}  ${path.basename(archive)}\n`);
  return archive;
}

if (require.main === module) packagePlatform();

module.exports = { packagePlatform };
