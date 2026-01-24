async function main() {
  const mod = await import('./export-default-module.js');
  console.log('module:', mod);
  console.log('default:', mod.default);
  if (typeof mod.default === 'function') {
    console.log(mod.default('world'));
  }
}

void main();
