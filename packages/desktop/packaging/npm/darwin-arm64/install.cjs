'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const packageRoot = __dirname;
const archive = path.join(packageRoot, 'artifacts', 'ant-desktop-darwin-arm64.tar.gz');
const checksumFile = `${archive}.sha256`;
const vendor = path.join(packageRoot, 'vendor');
const executable = path.join(vendor, 'runtime', 'Ant Desktop.app', 'Contents', 'MacOS', 'Ant Desktop');
const framework = path.join(vendor, 'runtime', 'Ant Desktop.app', 'Contents', 'Frameworks', 'Chromium Embedded Framework.framework');

function digest(filename) {
  const hash = crypto.createHash('sha256');
  hash.update(fs.readFileSync(filename));
  return hash.digest('hex');
}

function install() {
  if (fs.existsSync(executable) && fs.existsSync(framework)) return;
  if (!fs.existsSync(archive) || !fs.existsSync(checksumFile)) {
    throw new Error('Ant Desktop platform archive is missing from the npm package');
  }
  const expected = fs.readFileSync(checksumFile, 'utf8').trim().split(/\s+/)[0];
  const actual = digest(archive);
  if (expected !== actual) {
    throw new Error(`Ant Desktop platform archive checksum mismatch: expected ${expected}, got ${actual}`);
  }
  fs.rmSync(vendor, { recursive: true, force: true });
  fs.mkdirSync(vendor, { recursive: true });
  const result = spawnSync('/usr/bin/tar', ['-xzf', archive, '-C', vendor], {
    stdio: 'inherit'
  });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`Could not unpack Ant Desktop platform archive (tar exited ${result.status})`);
  }
  fs.chmodSync(executable, 0o755);
}

if (require.main === module) install();

module.exports = { install };
