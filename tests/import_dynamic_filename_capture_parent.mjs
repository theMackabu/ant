function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const topFilename = import.meta.filename;

async function loadChild() {
  return await import('./import_dynamic_filename_capture_child.mjs');
}

const child = await loadChild();

assert(import.meta.filename === topFilename, 'parent import.meta.filename drifted');
assert(
  child.childFilename.endsWith('/tests/import_dynamic_filename_capture_child.mjs'),
  `dynamic import resolved from wrong base: ${child.childFilename}`
);

console.log(`import.dynamic.filename.capture:${topFilename}`);
