const a = require('./fixtures/cjs-cycle-export-a.cjs');

if (typeof a.applyToClass !== 'function') {
  throw new Error('final module.exports should be the reassigned class');
}

if (a.seenType !== 'function' || a.seenValue !== 'ok') {
  throw new Error('circular require should see reassigned module.exports');
}

console.log('commonjs:circular-module-exports:ok');
