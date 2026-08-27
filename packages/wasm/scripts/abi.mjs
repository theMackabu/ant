import { readFile } from 'node:fs/promises';

export async function readAbi(path = new URL('../abi.json', import.meta.url)) {
  const abi = JSON.parse(await readFile(path, 'utf8'));
  for (const [name, value] of Object.entries(abi)) {
    if (!/^[A-Z][A-Z0-9_]*$/.test(name) || !Number.isSafeInteger(value) || value < 0 || value > 0xffffffff) {
      throw new TypeError(`Invalid WebAssembly ABI constant ${name}`);
    }
  }
  return abi;
}

export function renderAbiHeader(abi) {
  const definitions = Object.entries(abi)
    .map(([name, value]) => `#define ANT_WASM_${name} UINT32_C(${value})`)
    .join('\n');
  return `#ifndef ANT_WASM_ABI_H\n#define ANT_WASM_ABI_H\n\n` + `#include <stdint.h>\n\n${definitions}\n\n#endif\n`;
}

export function renderAbiModule(abi) {
  return (
    Object.entries(abi)
      .map(([name, value]) => `export const ${name} = ${value};`)
      .join('\n') + '\n'
  );
}
