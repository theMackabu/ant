import { spawnSync } from 'node:child_process';

const [, , compiler, ...compilerArguments] = process.argv;
if (!compiler) throw new Error('The Clang executable was not provided');

// Meson uses the complete `clang --version` output to decide whether Clang
// targets Windows. The Windows wasi-sdk archive name appears in InstalledDir,
// which makes Meson select lld-link even though this compiler targets WASI.
// The first line contains everything Meson needs to identify and version Clang.
const isCompilerVersionProbe = compilerArguments.at(-1) === '--version';

// Meson classifies wasi-sdk's wasm-ld as a GNU linker and adds archive group
// flags. wasm-ld resolves archives to a fixed point without those flags and
// deliberately does not accept them.
const argumentsWithoutGroups = compilerArguments.filter(argument => argument !== '-Wl,--start-group' && argument !== '-Wl,--end-group');
const result = spawnSync(compiler, argumentsWithoutGroups, isCompilerVersionProbe ? { encoding: 'utf8' } : { stdio: 'inherit' });

if (result.error) throw result.error;
if (isCompilerVersionProbe) {
  const [version] = result.stdout.split(/\r?\n/, 1);
  if (version) process.stdout.write(`${version}\n`);
  if (result.stderr) process.stderr.write(result.stderr);
}

if (result.signal) process.kill(process.pid, result.signal);
process.exit(result.status ?? 1);
