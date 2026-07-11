#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const { execute } = require('./lib/command.cjs');
const {
  assertHostMatches,
  readRuntimeLock
} = require('./lib/runtime-lock.cjs');
const { fetchBrowserRuntime } = require('./fetch-browser-runtime.cjs');

const desktopRoot = path.resolve(__dirname, '..');
const repositoryRoot = path.resolve(desktopRoot, '../..');
const libant = path.join(repositoryRoot, 'packages', 'libant', 'dist');

function findExecutable(name) {
  for (const directory of (process.env.PATH || '').split(path.delimiter)) {
    if (!directory) continue;
    const candidate = path.join(directory, name);
    if (fs.existsSync(candidate)) return candidate;
  }
  return name;
}

async function buildBrowserHost() {
  if (!fs.existsSync(path.join(libant, 'libant.a')) ||
      !fs.existsSync(path.join(libant, 'ant.h'))) {
    execute(path.join(repositoryRoot, 'packages', 'libant', 'build.sh'), []);
  }
  const lock = readRuntimeLock();
  const target = assertHostMatches(lock);
  const cefRoot = await fetchBrowserRuntime();
  const cCompiler = process.env.CC || findExecutable('clang');
  const cxxCompiler = process.env.CXX || findExecutable('clang++');
  const build = path.join(desktopRoot, 'build', 'browser', 'cef');
  const product = path.join(build, 'Release', 'Ant Desktop.app');
  execute(
    'cmake',
    [
      '-S',
      'browser/cef',
      '-B',
      build,
      '-G',
      'Xcode',
      `-DCEF_ROOT=${cefRoot}`,
      `-DLIBANT_ROOT=${libant}`,
      `-DCMAKE_C_COMPILER=${cCompiler}`,
      `-DCMAKE_CXX_COMPILER=${cxxCompiler}`,
      `-DPROJECT_ARCH=${target.cmakeArch}`,
      '-DUSE_SANDBOX=ON'
    ],
    { cwd: desktopRoot }
  );
  execute('cmake', ['--build', build, '--target', 'ant_chromium_runtime_stage', '--config', 'Release', '--', '-quiet'], { cwd: desktopRoot });

  const runtime = path.join(desktopRoot, 'runtime');
  fs.rmSync(runtime, { recursive: true, force: true });
  fs.mkdirSync(runtime, { recursive: true });
  execute('ditto', [product, path.join(runtime, 'Ant Desktop.app')]);
  process.stdout.write(`Installed embedded Chromium runtime in ${runtime}\n`);
  return runtime;
}

if (require.main === module) {
  buildBrowserHost().catch(error => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}

module.exports = { buildBrowserHost };
