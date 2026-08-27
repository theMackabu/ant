import { readAbi } from './abi.mjs';

const [, , path, name] = process.argv;
const abi = await readAbi(path);
if (!(name in abi)) throw new TypeError(`Unknown WebAssembly ABI constant ${name}`);
process.stdout.write(String(abi[name]));
