import { createHash } from 'node:crypto';
import { createReadStream, createWriteStream } from 'node:fs';
import { access, cp, mkdir, readFile, readdir, rm, symlink, writeFile } from 'node:fs/promises';
import { Readable } from 'node:stream';
import { pipeline } from 'node:stream/promises';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { readAbi, renderAbiHeader, renderAbiModule } from './abi.mjs';

const WASI_SDK_RELEASE = '34';
const WASI_SDK_VERSION = '34.0';

const WASI_SDK_ASSETS = {
  'darwin-arm64': ['arm64-macos', '9c59398106b417f8f14913380fdf0097a8cc0ff4af9eb3ce0065a859e88d49e9'],
  'darwin-x64': ['x86_64-macos', '87d27fa8adc68dee59bfbf2e22a6d34ef717c34d6bf1d8af2a56fc929d9ce0eb'],
  'linux-arm64': ['arm64-linux', 'f7e243dff54d60bcc576e94d6166b69f410f2500ae4a9ceef34315be10e77971'],
  'linux-x64': ['x86_64-linux', 'b761e3a0721dbae9c09a0059e5fdb2bf917d1b4a8a7b430fb3b5aafb0984b2c4'],
  'win32-arm64': ['arm64-windows', '45e1c71f3e965621e7b98ebe1d37b0e4b1f77f3e8072113ffb4534e67b1a4b7c'],
  'win32-x64': ['x86_64-windows', 'cccb5c323a9b34f0349a9b09e8804a0a7632c68c3310f4b5f437ed57d7e71d8f']
};

const packageDirectory = fileURLToPath(new URL('..', import.meta.url));
const buildDirectory = fileURLToPath(new URL('../build', import.meta.url));
const cacheDirectory = fileURLToPath(new URL('../.cache', import.meta.url));
const distDirectory = fileURLToPath(new URL('../dist', import.meta.url));
const packageVendorDirectory = fileURLToPath(new URL('../vendor', import.meta.url));
const repositoryVendorDirectory = fileURLToPath(new URL('../../../vendor', import.meta.url));
const generatedDirectory = fileURLToPath(new URL('../.cache/generated', import.meta.url));
const executableSuffix = process.platform === 'win32' ? '.exe' : '';

const EXPECTED_IMPORTS = [
  'ant.host_call:function',
  'ant.now_ms:function',
  'ant.random_fill:function',
  'env.memory:memory',
  'wasi_snapshot_preview1.environ_get:function',
  'wasi_snapshot_preview1.environ_sizes_get:function',
  'wasi_snapshot_preview1.fd_close:function',
  'wasi_snapshot_preview1.fd_prestat_dir_name:function',
  'wasi_snapshot_preview1.fd_prestat_get:function',
  'wasi_snapshot_preview1.fd_seek:function',
  'wasi_snapshot_preview1.fd_write:function',
  'wasi_snapshot_preview1.proc_exit:function',
  'wasi_snapshot_preview1.random_get:function'
].sort();

const EXPECTED_EXPORTS = [
  '_initialize:function',
  'ant_alloc:function',
  'ant_create:function',
  'ant_destroy:function',
  'ant_eval:function',
  'ant_free:function',
  'ant_release_result:function',
  'ant_result_length:function',
  'ant_set_global:function'
].sort();

function run(command, args) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, {
      cwd: packageDirectory,
      stdio: 'inherit'
    });
    child.on('error', reject);
    child.on('exit', code => {
      if (code === 0) resolve();
      else reject(new Error(`${command} exited with status ${code}`));
    });
  });
}

async function exists(path) {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}

async function sha256(path) {
  const hash = createHash('sha256');
  for await (const chunk of createReadStream(path)) hash.update(chunk);
  return hash.digest('hex');
}

async function ensureVendorDirectory() {
  if (await exists(packageVendorDirectory)) return;
  await symlink(repositoryVendorDirectory, packageVendorDirectory, process.platform === 'win32' ? 'junction' : 'dir');
}

async function findExtractedSdk(assetName) {
  const exact = `${cacheDirectory}/wasi-sdk-${WASI_SDK_VERSION}`;
  if (await exists(exact)) return exact;

  const entries = await readdir(cacheDirectory, { withFileTypes: true });
  const prefix = `wasi-sdk-${WASI_SDK_VERSION}`;
  const entry = entries.find(item => item.isDirectory() && item.name.startsWith(prefix) && item.name.includes(assetName));
  return entry ? `${cacheDirectory}/${entry.name}` : null;
}

async function resolveWasiSdk() {
  if (process.env.WASI_SDK_PATH) return process.env.WASI_SDK_PATH;

  const platformKey = `${process.platform}-${process.arch}`;
  const asset = WASI_SDK_ASSETS[platformKey];
  if (!asset) {
    throw new Error(`No pinned wasi-sdk build for ${platformKey}; set WASI_SDK_PATH to a wasi-sdk ${WASI_SDK_VERSION} installation`);
  }

  const [assetName, expectedDigest] = asset;
  await mkdir(cacheDirectory, { recursive: true });
  const sdk = await findExtractedSdk(assetName);
  if (sdk) return sdk;

  const filename = `wasi-sdk-${WASI_SDK_VERSION}-${assetName}.tar.gz`;
  const archive = `${cacheDirectory}/${filename}`;
  if (!(await exists(archive)) || (await sha256(archive)) !== expectedDigest) {
    const url = `https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_RELEASE}/${filename}`;
    const response = await fetch(url);
    if (!response.ok || !response.body) {
      throw new Error(`Unable to download ${url}: HTTP ${response.status}`);
    }
    await pipeline(Readable.fromWeb(response.body), createWriteStream(archive));
  }

  const digest = await sha256(archive);
  if (digest !== expectedDigest) {
    throw new Error(`SHA-256 mismatch for ${filename}: expected ${expectedDigest}, received ${digest}`);
  }

  await run('tar', ['-xzf', archive, '-C', cacheDirectory]);
  const extracted = await findExtractedSdk(assetName);
  if (!extracted) throw new Error(`Unable to find the extracted ${filename}`);
  return extracted;
}

function mesonString(value) {
  return `'${value.replaceAll('\\', '/').replaceAll("'", "\\'")}'`;
}

async function writeCrossFile(sdk) {
  const binary = name => `${sdk}/bin/${name}${executableSuffix}`;
  const sysroot = `${sdk}/share/wasi-sysroot`;
  const crossFile = `${cacheDirectory}/wasi-sdk.ini`;
  const clangWrapper = fileURLToPath(new URL('./clang.mjs', import.meta.url));
  const compiler = name =>
    `[${mesonString(process.execPath)}, ${mesonString(clangWrapper)}, ${mesonString(binary(name))}, ${mesonString('--target=wasm32-wasip1')}, ${mesonString(`--sysroot=${sysroot}`)}]`;

  await writeFile(
    crossFile,
    `[binaries]\n` +
      `c = ${compiler('clang')}\n` +
      `cpp = ${compiler('clang++')}\n` +
      `ar = ${mesonString(binary('llvm-ar'))}\n` +
      `strip = ${mesonString(binary('llvm-strip'))}\n\n` +
      `[host_machine]\n` +
      `system = 'wasi'\n` +
      `cpu_family = 'wasm32'\n` +
      `cpu = 'wasm32'\n` +
      `endian = 'little'\n\n` +
      `[properties]\n` +
      `needs_exe_wrapper = true\n`
  );
  return crossFile;
}

async function verifyModuleContract(path) {
  const module = await WebAssembly.compile(await readFile(path));
  const imports = WebAssembly.Module.imports(module)
    .map(item => `${item.module}.${item.name}:${item.kind}`)
    .sort();
  const exports = WebAssembly.Module.exports(module)
    .map(item => `${item.name}:${item.kind}`)
    .sort();
  if (JSON.stringify(imports) !== JSON.stringify(EXPECTED_IMPORTS)) {
    throw new Error(`Unexpected WebAssembly imports:\n${imports.join('\n')}`);
  }
  if (JSON.stringify(exports) !== JSON.stringify(EXPECTED_EXPORTS)) {
    throw new Error(`Unexpected WebAssembly exports:\n${exports.join('\n')}`);
  }
}

const abi = await readAbi();
await mkdir(generatedDirectory, { recursive: true });
await writeFile(`${generatedDirectory}/wasm_abi.h`, renderAbiHeader(abi));
await ensureVendorDirectory();

const sdk = await resolveWasiSdk();
const crossFile = await writeCrossFile(sdk);
const configurationStamp = `${buildDirectory}/.ant-wasi-toolchain`;
const configuration = JSON.stringify({ abi, node: process.execPath, sdk, target: 'wasm32-wasip1' });

let configured = false;
try {
  configured = (await readFile(configurationStamp, 'utf8')) === configuration;
  await access(`${buildDirectory}/meson-private/coredata.dat`);
} catch {
  configured = false;
}

if (!configured) await rm(buildDirectory, { recursive: true, force: true });
await run('meson', ['setup', ...(configured ? ['--reconfigure'] : []), buildDirectory, '--cross-file', crossFile]);
await writeFile(configurationStamp, configuration);
await run('meson', ['compile', '-C', buildDirectory]);
await run(`${sdk}/bin/llvm-strip${executableSuffix}`, [`${buildDirectory}/ant.wasm`]);
await verifyModuleContract(`${buildDirectory}/ant.wasm`);

await rm(distDirectory, { recursive: true, force: true });
await mkdir(distDirectory, { recursive: true });
await mkdir(`${distDirectory}/licenses`, { recursive: true });
await writeFile(`${distDirectory}/abi.js`, renderAbiModule(abi));

await Promise.all([
  cp(new URL('../src/index.js', import.meta.url), new URL('../dist/index.js', import.meta.url)),
  cp(new URL('../src/index.d.ts', import.meta.url), new URL('../dist/index.d.ts', import.meta.url)),
  cp(new URL('../build/ant.wasm', import.meta.url), new URL('../dist/ant.wasm', import.meta.url)),
  cp(new URL('../licenses/themackabu.txt', import.meta.url), new URL('../dist/licenses/themackabu.txt', import.meta.url)),
  cp(new URL('../../../vendor/base64-0.5.2/LICENSE', import.meta.url), new URL('../dist/licenses/base64.txt', import.meta.url)),
  cp(new URL('../../../vendor/pcre2-10.47/COPYING', import.meta.url), new URL('../dist/licenses/pcre2.txt', import.meta.url)),
  cp(new URL('../../../vendor/utf8proc-2.10.0/LICENSE.md', import.meta.url), new URL('../dist/licenses/utf8proc.md', import.meta.url)),
  cp(new URL('../../../vendor/uthash-2.3.0/LICENSE', import.meta.url), new URL('../dist/licenses/uthash.txt', import.meta.url)),
  cp(new URL('../../../vendor/crprintf/LICENSE.txt', import.meta.url), new URL('../dist/licenses/crprintf.txt', import.meta.url)),
  cp(new URL('../../../vendor/double-conversion-3.4.0/LICENSE', import.meta.url), new URL('../dist/licenses/double-conversion.txt', import.meta.url)),
  cp(new URL('../../../vendor/yyjson-0.12.0/LICENSE', import.meta.url), new URL('../dist/licenses/yyjson.txt', import.meta.url)),
  cp(new URL('../licenses/wasi-libc.txt', import.meta.url), new URL('../dist/licenses/wasi-libc.txt', import.meta.url))
]);
