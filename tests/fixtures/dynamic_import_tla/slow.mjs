globalThis.dynamicImportTlaLog.push('slow start');
await new Promise((resolve) => setTimeout(resolve, 5));
export const value = 'ready';
globalThis.dynamicImportTlaLog.push('slow end');
