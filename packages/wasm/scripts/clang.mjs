import { spawnSync } from 'node:child_process';

const [, , compiler, ...compilerArguments] = process.argv;
if (!compiler) throw new Error('The Clang executable was not provided');

// Meson classifies wasi-sdk's wasm-ld as a GNU linker and adds archive group
// flags. wasm-ld resolves archives to a fixed point without those flags and
// deliberately does not accept them.
const argumentsWithoutGroups = compilerArguments.filter(argument => argument !== '-Wl,--start-group' && argument !== '-Wl,--end-group');
const result = spawnSync(compiler, argumentsWithoutGroups, { stdio: 'inherit' });

if (result.error) throw result.error;
if (result.signal) process.kill(process.pid, result.signal);
process.exit(result.status ?? 1);
