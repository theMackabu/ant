// JIT tier differential: random arithmetic and control-flow kernels run hot
// enough to reach the template JIT and the optimizing tier, then with
// boundary and mistyped arguments that force bailouts. Node is the oracle,
// and every Ant variant (interpreter, template JIT only, forced and verified
// optimizing tier) must print what Node prints. Kernels are trees, so
// fast-check shrinks a failure to a small expression.

import fc from 'fast-check';
import type { Variant } from '../engine.ts';
import { finish, literal } from '../prelude.ts';
import { defineFamily } from './family.ts';

const WARM_CALLS = 3000;

// op-warn also traces every compile and bailout, so stderr is not compared;
// only lines matching `stderrFailures` count as mismatches.
const stderrFailures = 'operand depth analysis failed|codegen-failed|verif(y|ier)[^\\n]*(fail|error)';
export const tierVariants: readonly Variant[] = [
  { name: 'default', env: { ANT_DEBUG: 'dump/vm:op-warn' }, stderrFailures },
  { name: 'jitless', args: ['--jitless'], env: { ANT_DEBUG: 'dump/vm:op-warn' }, stderrFailures },
  { name: 'opt:off', env: { ANT_DEBUG: 'opt:off dump/vm:op-warn' }, stderrFailures },
  { name: 'opt:force', env: { ANT_DEBUG: 'opt:force,verify dump/vm:op-warn' }, stderrFailures },
  { name: 'opt:verify', env: { ANT_DEBUG: 'opt:verify dump/vm:op-warn' }, stderrFailures }
];

const leaf = fc.constantFrom(
  'a', 'b', 'c', 'r',
  '0', '1', '-1', '2', '3', '7', '31', '32', '255', '65535', '0x3fffffff', '0x7fffffff', '-0x80000000', '1e9',
  '0.5', '-0', '1.5', '-2.25', '1e21', '1e-7', 'NaN', 'Infinity'
);
const binaryOp = fc.constantFrom('+', '-', '*', '/', '%', '|', '&', '^', '<<', '>>', '>>>');
const compareOp = fc.constantFrom('<', '<=', '>', '>=', '===', '!==', '==', '!=');
// only exactly specified Math functions: atan2, pow and the like may differ in the last bit
const call2 = fc.constantFrom('Math.imul', 'Math.max', 'Math.min');
const call1 = fc.constantFrom('Math.floor', 'Math.ceil', 'Math.round', 'Math.trunc', 'Math.abs', 'Math.sign', 'Math.sqrt', 'Math.clz32', 'Math.fround');

const { expr } = fc.letrec<{ expr: string }>(tie => ({
  expr: fc.oneof(
    // fast-check biases toward shallow trees; JIT bugs need some depth
    { maxDepth: 4, depthSize: 'medium', depthIdentifier: 'expr' },
    leaf,
    fc.tuple(tie('expr'), binaryOp, tie('expr')).map(([l, op, r]) => `(${l} ${op} ${r})`),
    fc.tuple(tie('expr'), compareOp, tie('expr'), tie('expr'), tie('expr')).map(([l, op, r, t, f]) => `(${l} ${op} ${r} ? ${t} : ${f})`),
    fc.tuple(call2, tie('expr'), tie('expr')).map(([fn, l, r]) => `${fn}(${l}, ${r})`),
    fc.tuple(call1, tie('expr')).map(([fn, x]) => `${fn}(${x})`),
    tie('expr').map(x => `(${x} | 0)`),
    // the space keeps - -1 from becoming the decrement --1
    fc.tuple(fc.constantFrom('-', '~', '+', '!'), tie('expr')).map(([op, x]) => `(${op} ${x})`),
    fc.tuple(tie('expr'), tie('expr')).map(([l, r]) => `(${l} ?? ${r})`),
    tie('expr').map(x => `String(${x}).length`)
  )
}));

// Statement shapes that stress loops, element and property access, and
// closures; each takes one generated expression.
const statementTemplates = [
  (e: string) => `for (let k = 0; k < (b & 7); k++) r = (r + k * ${e}) | 0;`,
  (e: string) => `{ const arr = [a, b, c, ${e}]; r = r + (arr[a & 7] ?? 1); }`,
  (e: string) => `{ const o = (a & 1) ? { x: a, y: b } : { y: b, x: a, z: c }; r = o.x - o.y + (o.z ?? ${e}); }`,
  (e: string) => `if (r !== r || r === Infinity || r === -Infinity) r = ${e};`,
  (e: string) => `{ const g = x => x * ${e} + r; r = g(a); }`,
  (e: string) => `{ let s = 0; for (const v of [a, b]) s += v; r = s - r + ${e}; }`,
  (e: string) => `switch (a & 3) { case 0: r += 1; break; case 1: r = ${e}; break; default: r -= b; }`,
  (e: string) => `{ const t = typed[a & 7]; typed[b & 7] = r; r = t + ${e}; }`,
  (e: string) => `try { if ((a & 15) === 3) throw r; } catch (err) { r = err + ${e}; }`,
  (e: string) => `r = (r > 1e6 || r < -1e6) ? r % 1000 : r + ${e};`
];
const statement = fc
  .tuple(fc.integer({ min: 0, max: statementTemplates.length - 1 }), expr)
  .map(([template, e]) => statementTemplates[template](e));

const warmArgs = ['[i % 100 - 50, (i * 7) % 33 + 1, i]', '[i, i & 15, (i * 13) % 7]', '[(i % 9) * 0.5, i % 4, -i]'];
const boundaryArg = fc.constantFrom(
  '0x7fffffff', '-0x80000000', '0x40000000', '-0', '0.5', 'NaN', 'Infinity', '-Infinity', '1e21', '2 ** 53',
  '"7"', '"x"', 'undefined', 'null', 'true', '[]', '{}', '10n', '1', '3', '-5'
);

const kernel = fc.record({
  initial: expr,
  statements: fc.array(statement, { maxLength: 3, size: 'max' }),
  returns: expr,
  warm: fc.constantFrom(...warmArgs),
  finals: fc.array(fc.tuple(boundaryArg, boundaryArg, fc.integer({ min: -3, max: 3 }).map(String)), { minLength: 1, maxLength: 16 })
});
type Kernel = typeof kernel extends fc.Arbitrary<infer K> ? K : never;

function kernelSource({ initial, statements, returns, warm, finals }: Kernel, n: number): string {
  return `function kernel${n}(a, b, c) {
  let r = ${initial};
  ${statements.join('\n  ')}
  return ${returns};
}
{
  let hash = 0;
  for (let i = 0; i < ${WARM_CALLS}; i++) {
    const [a, b, c] = ${warm};
    let v;
    try { v = kernel${n}(a, b, c); } catch (e) { v = 'throws ' + e.name; }
    const s = key(v);
    for (let k = 0; k < s.length; k++) hash = (Math.imul(hash, 31) + s.charCodeAt(k)) | 0;
  }
  __record(${literal(`warm:${n}`)}, () => hash);
}
${finals.map((args, i) => `__recordName(${literal(`call:${n}:${i}:${args.join(',')}`)}, () => kernel${n}(${args.join(', ')}));`).join('\n')}
__record(${literal(`typed:${n}`)}, () => Array.from(typed));`;
}

// Several kernels per probe: process start dominates the cost of a case, and
// shrinking drops the kernels a mismatch does not need.
export const jit = defineFamily({
  name: 'jit',
  variants: tierVariants,
  arbitrary: fc.record({ kernels: fc.array(kernel, { minLength: 1, maxLength: 4, size: 'max' }) }),
  build({ kernels }) {
    return finish(`const typed = new Int32Array(8);
function key(v) { return typeof v === 'number' && Object.is(v, -0) ? '-0' : typeof v + ':' + String(v); }
${kernels.map(kernelSource).join('\n')}`);
  }
});
