#!/usr/bin/env ant

import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const desktopRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repositoryRoot = path.resolve(desktopRoot, '../..');
const platforms = require(path.join(desktopRoot, 'platforms.json'));
const system = platforms.names[process.platform];
const platformRoot = path.join(desktopRoot, 'packaging', 'npm', `${system}-${process.arch}`);
if (!fs.existsSync(path.join(platformRoot, 'cef.lock.json'))) {
  throw new Error(`Ant Desktop has no browser runtime lock for ${process.platform}-${process.arch}`);
}
const lock = require(path.join(platformRoot, 'cef.lock.json'));
const { version } = require(path.join(desktopRoot, 'package.json'));

const FRAMEWORK = 'Chromium Embedded Framework.framework';
const HELPER = 'Ant Desktop Helper';
const HELPER_KINDS = ['', ' (Alerts)', ' (GPU)', ' (Plugin)', ' (Renderer)'];

function run(command, args, cwd = desktopRoot) {
  const result = spawnSync(command, args, { cwd, stdio: 'inherit' });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} exited with status ${result.status}`);
}

async function fetchCef() {
  const cache = path.join(desktopRoot, '.cache');
  const archive = path.join(cache, lock.archive);
  const source = archive.replace(/\.tar\.bz2$/, '');
  fs.mkdirSync(cache, { recursive: true });

  if (!fs.existsSync(archive)) {
    process.stdout.write(`Downloading ${lock.archive}\n`);
    const response = await fetch(`https://cef-builds.spotifycdn.com/${encodeURIComponent(lock.archive)}`);
    if (!response.ok) throw new Error(`CEF download failed with HTTP ${response.status}`);
    const temporary = `${archive}.download-${process.pid}`;
    fs.writeFileSync(temporary, Buffer.from(await response.arrayBuffer()));
    fs.renameSync(temporary, archive);
  }

  const contents = fs.readFileSync(archive);
  const sha1 = crypto.createHash('sha1').update(contents).digest('hex');
  if (contents.length !== lock.size || sha1 !== lock.sha1) {
    throw new Error(`CEF archive does not match cef.lock.json (size ${contents.length}, sha1 ${sha1})`);
  }
  if (!fs.existsSync(source)) run('tar', ['-xjf', archive, '-C', cache]);
  return source;
}

function sharedCefRoot() {
  if (system === 'darwin') return path.join(os.homedir(), 'Library', 'Application Support', 'Ant Desktop', 'CEF');
  if (system === 'windows') return path.join(process.env.LOCALAPPDATA, 'Ant Desktop', 'CEF');
  return path.join(process.env.XDG_DATA_HOME || path.join(os.homedir(), '.local', 'share'), 'ant-desktop', 'cef');
}

function installSharedCef(source) {
  const shared = path.join(sharedCefRoot(), lock.cefVersion);
  const marker = { darwin: FRAMEWORK, windows: 'libcef.dll', linux: 'libcef.so' }[system];
  if (fs.existsSync(path.join(shared, marker))) return;
  fs.mkdirSync(shared, { recursive: true });
  if (system === 'darwin') run('/bin/cp', ['-Rc', path.join(source, 'Release', FRAMEWORK), shared]);
  else for (const part of ['Release', 'Resources']) fs.cpSync(path.join(source, part), shared, { recursive: true });
  process.stdout.write(`Installed the shared browser runtime in ${shared}\n`);
}

function ensureLibant() {
  const build = process.env.ANT_EMBED_BUILD || path.join(repositoryRoot, 'build-embed');
  if (!fs.existsSync(path.join(build, 'build.ninja'))) run('meson', ['setup', build, '-Db_lto=false'], repositoryRoot);
  run('meson', ['compile', '-C', build, 'embed_dist'], repositoryRoot);
  return path.join(build, 'libant');
}

function compile(cef, libant) {
  const build = path.join(desktopRoot, 'build');
  const tool = name => spawnSync('xcrun', ['--find', name], { encoding: 'utf8' }).stdout.trim();
  const compilers = {
    darwin: () => [tool('clang'), tool('clang++')],
    windows: () => [process.env.CC || 'clang-cl', process.env.CXX || 'clang-cl'],
    linux: () => [process.env.CC || 'clang', process.env.CXX || 'clang++']
  };
  const [cc, cxx] = compilers[system]();
  run('cmake', [
    '-S', desktopRoot, '-B', build, '-G', 'Ninja',
    `-DCMAKE_C_COMPILER=${cc}`,
    `-DCMAKE_CXX_COMPILER=${cxx}`,
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    `-DPROJECT_ARCH=${process.arch === 'x64' ? 'x86_64' : process.arch}`,
    `-DCEF_ROOT=${cef}`,
    `-DLIBANT_ROOT=${libant}`,
    `-DDESKTOP_VERSION=${version}`,
    `-DCEF_VERSION=${lock.cefVersion}`,
    `-DCEF_ARCHIVE=${lock.archive}`,
    `-DCEF_SHA1=${lock.sha1}`,
    `-DCEF_SIZE=${lock.size}`,
    `-DCHROMIUM_VERSION=${lock.chromiumVersion}`
  ]);
  run('cmake', ['--build', build]);
  return build;
}

function writeBundle(root, name, identifier, executable) {
  const { infoPlist } = require(path.join(desktopRoot, 'platform', 'darwin.cjs'));
  const contents = path.join(root, 'Contents');
  fs.mkdirSync(path.join(contents, 'MacOS'), { recursive: true });
  fs.copyFileSync(executable, path.join(contents, 'MacOS', name));
  fs.chmodSync(path.join(contents, 'MacOS', name), 0o755);
  fs.writeFileSync(
    path.join(contents, 'Info.plist'),
    infoPlist({ name, executable: name, identifier, version, background: true })
  );
  return contents;
}

function assembleDirectory(build) {
  const app = path.join(desktopRoot, 'runtime', 'ant-desktop');
  const files = system === 'windows' ? ['ant-desktop.exe', 'antdesktop.dll'] : ['ant-desktop', 'libantdesktop.so'];
  fs.rmSync(app, { recursive: true, force: true });
  fs.mkdirSync(app, { recursive: true });
  for (const file of files) fs.copyFileSync(path.join(build, file), path.join(app, file));
  process.stdout.write(`Built ${app}\n`);
}

function assemble(build) {
  if (system !== 'darwin') return assembleDirectory(build);
  const app = path.join(desktopRoot, 'runtime', 'Ant Desktop.app');
  const launcher = path.join(build, 'ant-desktop');
  fs.rmSync(app, { recursive: true, force: true });
  const frameworks = path.join(writeBundle(app, 'Ant Desktop', 'org.antjs.desktop', launcher), 'Frameworks');
  fs.mkdirSync(frameworks);
  fs.copyFileSync(path.join(build, 'libantdesktop.dylib'), path.join(frameworks, 'libantdesktop.dylib'));
  for (const kind of HELPER_KINDS) {
    const name = `${HELPER}${kind}`;
    const suffix = kind.replace(/[^A-Za-z]/g, '').toLowerCase();
    writeBundle(path.join(frameworks, `${name}.app`), name, `org.antjs.desktop.helper${suffix && `.${suffix}`}`, launcher);
  }
  process.stdout.write(`Built ${app}\n`);
}

const cef = await fetchCef();
installSharedCef(cef);
assemble(compile(cef, ensureLibant()));
