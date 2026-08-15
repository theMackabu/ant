import assert from 'node:assert';

export class NamedExport {}
export default class DefaultExport {}
export const ExpressionExport = class InnerExpression {};

assert.strictEqual(typeof NamedExport, 'function');
assert.strictEqual(typeof DefaultExport, 'function');
assert.strictEqual(typeof ExpressionExport, 'function');
assert.strictEqual(typeof InnerExpression, 'undefined');

console.log('class export scope ok');
