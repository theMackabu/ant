(async () => {
  const mod = await import('./ts_js_extension_resolution_main.ts');
  if (mod.default !== 'ts-source-fallback-ok') {
    throw new Error(`expected ts-source-fallback-ok, got ${String(mod.default)}`);
  }
  console.log('ok');
})().catch(err => {
  console.error(err);
  process.exit(1);
});
