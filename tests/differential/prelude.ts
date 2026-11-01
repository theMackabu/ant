// Probe source helpers. Every probe records [label, value] pairs and prints
// them as one JSON line, which the runner compares between engines.

export const literal = (value: unknown): string => JSON.stringify(value);

// Special values are encoded as "$"-prefixed strings (and "$" strings are
// escaped as "$$"), not as object literals: __canon runs hot, and object
// literals in hot functions are exactly what JIT bugs corrupt. Those belong
// to the jit family, not to every family's bookkeeping.
const prelude = String.raw`
const __out = [];
function __canon(value, seen = new Set()) {
  if (value === undefined) return '$undefined';
  if (typeof value === 'string') return value[0] === '$' ? '$' + value : value;
  if (typeof value === 'number') {
    if (Number.isNaN(value)) return '$NaN';
    if (value === Infinity) return '$Infinity';
    if (value === -Infinity) return '$-Infinity';
    if (Object.is(value, -0)) return '$-0';
    return value;
  }
  if (typeof value === 'bigint') return '$bigint:' + String(value);
  if (typeof value === 'symbol') return '$symbol:' + String(value);
  if (typeof value === 'function') return '$function:' + value.name + '/' + value.length;
  if (value === null || typeof value !== 'object') return value;
  if (seen.has(value)) return '$circular';
  if (value instanceof Error) return '$error:' + value.name + ':' + value.message;
  seen.add(value);
  const result = Array.isArray(value) ? [] : {};
  for (const key of Object.keys(value).sort()) result[key] = __canon(value[key], seen);
  seen.delete(value);
  return result;
}
function __record(label, callback) {
  try { __out.push([label, __canon(callback())]); }
  catch (error) { __out.push([label, __canon(error)]); }
}
// error messages differ between engines; only the error type is compared
function __recordName(label, callback) {
  try { __out.push([label, __canon(callback())]); }
  catch (error) { __out.push([label, '$error:' + (error && error.name)]); }
}
`;

export function finish(body: string, asynchronous = false): string {
  if (asynchronous) {
    return `${prelude}\n(async () => {\n${body}\nconsole.log(JSON.stringify(__out));\n})().catch(error => { console.log(JSON.stringify([['$fatal', __canon(error)]])); process.exitCode = 1; });\n`;
  }
  return `${prelude}\n${body}\nconsole.log(JSON.stringify(__out));\n`;
}

// Source text for a number, including the values String() loses.
export function numberSource(value: number): string {
  if (Number.isNaN(value)) return 'NaN';
  if (value === Infinity) return 'Infinity';
  if (value === -Infinity) return '-Infinity';
  if (Object.is(value, -0)) return '-0';
  return String(value);
}
