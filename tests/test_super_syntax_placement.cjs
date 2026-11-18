const assert = require('node:assert');

const misplaced = [
  'class C extends Object { m() { super = 5; } }',
  'class C extends Object { m() { super += 1; } }',
  'class C extends Object { m() { [super] = [1]; } }',
  'class C extends Object { m() { return typeof super; } }',
  'class C extends Object { x = super(); }',
  'class C extends Object { x = () => super(); }',
  'super;',
  'class C extends Object { constructor() { var s = super; } }',
  'class C extends Object { constructor() { new super(); } }',
  'class C extends Object { m() { return super?.x; } }',
  'class C { static m() { return super(); } }',
  'class C { m() { super(); } }',
  'class C { constructor() { super(); } }',
];
for (const source of misplaced) assert.throws(() => new Function(source), SyntaxError, source);

const allowed = [
  'class C extends Object { x = super.toString; constructor() { const call = () => super(); call(); } }',
  'class C extends Object { m() { return typeof super.x + super["y"] + (super.z = 1) + super.q++; } }',
  'class C extends Object { m() { return super.x`a`; } }',
  '({ m() { return super.x; } })',
  'class C extends Object { constructor() { const a = () => super(); a(); } m() { return () => super.x; } static s = super.x; x = super.y; static { super.z; } }',
  'class C { x = () => super.y; static #p() { return super.q; } }',
];
for (const source of allowed) assert.doesNotThrow(() => new Function(source), source);

console.log('ok');
