#!/usr/bin/env node
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const { execute } = require('./lib/command.cjs');
const { embedRendererBridge } = require('./embed-renderer-bridge.cjs');
const { readRuntimeLock } = require('./lib/runtime-lock.cjs');

const desktopRoot = path.resolve(__dirname, '..');
const repositoryRoot = path.resolve(desktopRoot, '../..');
const libant = path.join(repositoryRoot, 'packages', 'libant', 'dist');
const sources = [
  'app/archive/archive.c',
  'app/api/app.c',
  'app/api/browser_window.c',
  'app/api/desktop_module.c',
  'app/api/ipc_main.c',
  'app/api/web_contents.c',
  'app/core/window_state.c',
  'app/platform/mac/browser_view.mm',
  'app/platform/mac/cef_callbacks.c',
  'app/platform/mac/cef_runtime.mm',
  'app/platform/mac/desktop_window.mm',
  'app/platform/mac/embedded_browser.mm',
  'app/platform/mac/event_loop.mm',
  'app/platform/mac/main.mm',
  'app/platform/mac/menu.mm',
  'app/platform/mac/native_menu.mm',
  'app/platform/mac/window_factory.mm',
  'app/platform/mac/window_options.mm',
  'app/runtime/ant_runtime.c',
  'browser/cef/app_scheme.cc',
  'browser/cef/ipc.cc'
];

function buildApp() {
  const desktopVersion = JSON.parse(
    fs.readFileSync(path.join(desktopRoot, 'package.json'), 'utf8')
  ).version;
  const runtime = readRuntimeLock();
  const cefRoot = path.join(desktopRoot, '.cache', path.basename(runtime.archive, '.tar.bz2'));
  const cefWrapper = path.join(
    desktopRoot,
    'build',
    'browser',
    'cef',
    'libcef_dll_wrapper',
    'Release',
    'libcef_dll_wrapper.a'
  );
  if (!fs.existsSync(cefWrapper)) execute(process.execPath, [path.join(__dirname, 'build-browser-host.cjs')]);
  if (!fs.existsSync(path.join(libant, 'libant.a')) || !fs.existsSync(path.join(libant, 'ant.h'))) {
    execute(path.join(repositoryRoot, 'packages', 'libant', 'build.sh'), []);
  }

  const build = path.join(desktopRoot, 'build');
  const objectRoot = path.join(build, 'obj');
  const generated = path.join(build, 'generated');
  fs.mkdirSync(build, { recursive: true });
  for (const entry of fs.readdirSync(build)) {
    if (entry.endsWith('.o')) fs.rmSync(path.join(build, entry));
  }
  fs.rmSync(objectRoot, { recursive: true, force: true });
  fs.mkdirSync(objectRoot, { recursive: true });
  embedRendererBridge(path.join(generated, 'renderer_bridge.h'));

  const compiler = execute('xcrun', ['--find', 'clang'], { capture: true });
  const linker = process.env.CXX || 'clang++';
  const sdk = execute('xcrun', ['--sdk', 'macosx', '--show-sdk-path'], {
    capture: true
  });
  const objects = [];
  for (const source of sources) {
    const object = path.join(
      objectRoot,
      `${source.replaceAll('/', '_').replaceAll('.', '_')}.o`
    );
    const language = source.endsWith('cef_runtime.mm') || source.endsWith('embedded_browser.mm')
      ? 'objective-c++'
      : source.endsWith('.mm')
        ? 'objective-c'
        : source.endsWith('.cc')
          ? 'c++'
          : 'c';
    const standard = language.endsWith('++') || language === 'c++' ? '-std=c++20' : '-std=c11';
    execute(
      compiler,
      [
        '-x',
        language,
        standard,
        '-fobjc-arc',
        '-mmacosx-version-min=15.0',
        '-isysroot',
        sdk,
        `-I${libant}`,
        `-I${path.join(repositoryRoot, 'include')}`,
        `-I${generated}`,
        `-I${cefRoot}`,
        '-DCEF_USE_SANDBOX',
        `-DANT_DESKTOP_VERSION="${desktopVersion}"`,
        `-DANT_DESKTOP_CHROMIUM_VERSION="${runtime.chromiumVersion}"`,
        source,
        '-c',
        '-o',
        object
      ],
      { cwd: desktopRoot }
    );
    objects.push(object);
  }

  const output = path.join(build, 'ant-desktop');
  execute(
    linker,
    [
      '-mmacosx-version-min=15.0',
      '-isysroot',
      sdk,
      ...objects,
      path.join(libant, 'libant.a'),
      cefWrapper,
      '-framework',
      'AppKit',
      '-framework',
      'QuartzCore',
      '-framework',
      'Security',
      '-framework',
      'CoreFoundation',
      '-framework',
      'Hypervisor',
      '-lpthread',
      '-o',
      output
    ],
    { cwd: desktopRoot }
  );
  const bundledExecutable = path.join(
    desktopRoot,
    'runtime',
    'Ant Desktop.app',
    'Contents',
    'MacOS',
    'Ant Desktop'
  );
  if (fs.existsSync(path.dirname(bundledExecutable))) {
    fs.copyFileSync(output, bundledExecutable);
    fs.chmodSync(bundledExecutable, 0o755);
  }
  process.stdout.write(`Built ${output}\n`);
  return output;
}

if (require.main === module) buildApp();

module.exports = { buildApp };
