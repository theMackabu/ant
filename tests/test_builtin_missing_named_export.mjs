function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const expectedMessage =
  "The requested module 'node:url' does not provide an export named '__antMissingExportForDisplayNameTest'";

let caught = null;

try {
  await import('./builtin_missing_named_export_importer.mjs');
} catch (err) {
  caught = err;
}

assert(caught, 'expected import to fail');
assert(caught.name === 'SyntaxError', `expected SyntaxError, got ${caught && caught.name}`);
assert(caught.message === expectedMessage,
  `unexpected message:\n${caught && caught.message}\n!=\n${expectedMessage}`);

console.log('builtin missing named export test passed');
