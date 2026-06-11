function assert(condition, message) {
  if (!condition) throw new Error(message);
}

// Writes to exported bindings must propagate to the module namespace (live
// bindings), even when the write happens inside a function called while a
// different module is mid-eval, or after all module evaluation finished.
import * as ns from './export_live_binding_exporter.mjs';
import { counter, bump, writeAll } from './export_live_binding_exporter.mjs';

function readCounter() {
  return counter;
}

assert(ns.counter === 0, `initial counter should be 0, got ${ns.counter}`);

bump();
bump();
assert(ns.counter === 2, `namespace read after bump: expected 2, got ${ns.counter}`);
assert(counter === 2, `direct import read after bump: expected 2, got ${counter}`);
assert(readCounter() === 2, `hoisted-function read after bump: expected 2, got ${readCounter()}`);

writeAll();
assert(ns.compound === 5, `compound assignment export: expected 5, got ${ns.compound}`);
assert(ns.destructured === 42, `destructuring assignment export: expected 42, got ${ns.destructured}`);
assert(ns.forwardLexicalAlias === 41,
  `forward lexical alias: expected 41, got ${ns.forwardLexicalAlias}`);
assert(ns.ForwardClassAlias.value === 42,
  `forward class alias: expected 42, got ${ns.ForwardClassAlias?.value}`);
assert(ns.alias === 99, `aliased export clause: expected 99, got ${ns.alias}`);
assert(ns.multi === 7 && ns.multiB === 7 && ns.multiC === 7,
  `multi-alias export must update every name: got ${ns.multi}/${ns.multiB}/${ns.multiC}`);
assert(ns.varBinding === 11, `export var live binding: expected 11, got ${ns.varBinding}`);
assert('deferredAlias' in ns && 'deferredAliasA' in ns && 'deferredAliasB' in ns,
  'deferred export var aliases should be published');
assert(ns.deferredAlias === undefined && ns.deferredAliasA === undefined && ns.deferredAliasB === undefined,
  `deferred export var aliases should start undefined: got ${ns.deferredAlias}/${ns.deferredAliasA}/${ns.deferredAliasB}`);

ns.swapDefault();
assert(ns.default() === 'swapped',
  `reassigned default-function binding must live-update: got ${ns.default()}`);

// after the microtask queue drains, no module eval is active; writes must
// still find the exporter's namespace through the function's module ctx
await new Promise(resolve => setTimeout(resolve, 0));
bump();
assert(ns.counter === 3, `post-eval bump: expected 3, got ${ns.counter}`);

console.log('export.live.binding:ok');
