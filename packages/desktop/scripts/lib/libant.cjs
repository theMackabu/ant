'use strict';

const fs = require('node:fs');
const path = require('node:path');

const { execute } = require('./command.cjs');

const repositoryRoot = path.resolve(__dirname, '../../../..');
const buildDir = process.env.ANT_EMBED_BUILD || path.join(repositoryRoot, 'build-embed');
const dist = path.join(buildDir, 'libant');
const archive = path.join(dist, 'lib', 'libant.a');
const pkgConfig = path.join(dist, 'lib', 'pkgconfig', 'libant.pc');

function ensureLibant() {
  if (fs.existsSync(archive) && fs.existsSync(pkgConfig)) return dist;
  if (!fs.existsSync(path.join(buildDir, 'build.ninja'))) {
    execute('meson', ['setup', buildDir, '-Db_lto=false'], { cwd: repositoryRoot });
  }
  execute('meson', ['compile', '-C', buildDir, 'embed_dist'], { cwd: repositoryRoot });
  return dist;
}

function libantVersion() {
  if (!fs.existsSync(pkgConfig)) return null;
  const match = fs.readFileSync(pkgConfig, 'utf8').match(/^Version: (.+)$/m);
  return match ? match[1] : null;
}

module.exports = { archive, dist, ensureLibant, libantVersion };
