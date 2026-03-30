function assert(condition, message) {
  if (!condition) throw new Error(message);
}

import { loadChildLater } from './import_dynamic_filename_capture_exporter.mjs';

const childFilename = await loadChildLater();

assert(
  childFilename.endsWith('/tests/import_dynamic_filename_capture_child.mjs'),
  `late dynamic import resolved from wrong base: ${childFilename}`
);

console.log(`import.dynamic.filename.late:${childFilename}`);
