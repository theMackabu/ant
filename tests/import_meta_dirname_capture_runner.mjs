function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const mod = await import('./nested/import_meta_dirname_capture_child.mjs');

const expectedDir = '/tests/nested';

assert(
  mod.topDirname.endsWith(expectedDir),
  `top-level import.meta.dirname drifted: ${mod.topDirname}`
);
assert(
  mod.nestedDirname.endsWith(expectedDir),
  `nested import.meta.dirname drifted: ${mod.nestedDirname}`
);
const asyncDirname = await mod.readAsyncDirname();
assert(
  asyncDirname.endsWith(expectedDir),
  `async import.meta.dirname drifted: ${asyncDirname}`
);

console.log(`import.meta.dirname.capture:${mod.topDirname}`);
