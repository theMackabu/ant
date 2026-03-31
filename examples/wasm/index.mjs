import { readFile } from 'ant:fs';

const [wasmFile, ...wasmArgs] = process.argv.slice(2);
if (!wasmFile) {
  console.error('Usage: ant wasm.mjs <file.wasm> [args...]');
  process.exit(1);
}

const bytes = await readFile(wasmFile);

const { instance } = await WebAssembly.instantiate(bytes, {
  wasi: { args: [wasmFile, ...wasmArgs] }
});

instance.exports._start();
