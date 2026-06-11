function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// Hoisted function declarations compile before import statements execute, so
// they must still see the resolved import bindings, not the raw namespace.
import makeDefault, { Widget, label } from './import_hoisted_capture_exporter.mjs';
import * as ns from './import_hoisted_capture_exporter.mjs';

function constructNamed() {
  return new Widget();
}

function readNamed() {
  return label;
}

function callDefault() {
  return makeDefault();
}

function readNamespace() {
  return ns;
}

assert(constructNamed().tag() === 'widget', 'hoisted function saw namespace instead of named class import');
assert(readNamed() === 'exported-label', `hoisted function read wrong named import: ${readNamed()}`);
assert(callDefault() === 'default-result', 'hoisted function saw namespace instead of default import');
assert(typeof readNamespace().Widget === 'function', 'hoisted function lost namespace import');

console.log('import.hoisted.capture:ok');
