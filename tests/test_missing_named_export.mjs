function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const expectedPath = import.meta.dirname + '/missing_named_export_target.mjs';
const expectedMessage =
  `The requested module '${expectedPath}' does not provide an export named 'createDebug'`;

let caught = null;

try {
  await import('./missing_named_export_importer.mjs');
} catch (err) {
  caught = err;
}

assert(caught, 'expected import to fail');
assert(caught.name === 'SyntaxError', `expected SyntaxError, got ${caught && caught.name}`);
assert(caught.message === expectedMessage,
  `unexpected message:\n${caught && caught.message}\n!=\n${expectedMessage}`);

console.log('missing named export test passed');
