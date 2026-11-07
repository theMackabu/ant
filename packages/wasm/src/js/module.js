let compiledModulePromise;

async function compileModule() {
  const url = new URL('./ant.wasm', import.meta.url);
  if (url.protocol === 'file:') {
    const { readFile } = await import('node:fs/promises');
    return WebAssembly.compile(await readFile(url));
  }

  const response = await fetch(url);
  if (!response.ok) throw new Error(`Unable to load ${url}: HTTP ${response.status}`);
  if (typeof WebAssembly.compileStreaming === 'function') {
    try {
      return await WebAssembly.compileStreaming(response.clone());
    } catch {}
  }
  return WebAssembly.compile(await response.arrayBuffer());
}

export function getCompiledModule() {
  compiledModulePromise ??= compileModule().catch(error => {
    compiledModulePromise = undefined;
    throw error;
  });
  return compiledModulePromise;
}

export async function getCrypto() {
  if (globalThis.crypto?.getRandomValues) return globalThis.crypto;
  if (typeof process !== 'undefined' && process.versions?.node) return (await import('node:crypto')).webcrypto;
  throw new WebAssembly.RuntimeError('Web Crypto is required by @antjs.org/wasm');
}
