const assert = require('assert');

function u32(value) {
  const out = [];
  do {
    let byte = value & 0x7f;
    value >>>= 7;
    if (value) byte |= 0x80;
    out.push(byte);
  } while (value);
  return out;
}

function str(value) {
  const bytes = Array.from(Buffer.from(value));
  return [...u32(bytes.length), ...bytes];
}

function section(id, data) {
  return [id, ...u32(data.length), ...data];
}

function moduleBytes(sections) {
  return new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0, ...sections.flat()]);
}

const typeI32 = section(1, [1, 0x60, 0, 1, 0x7f]);
const importsWithCallbackAndTable = section(2, [
  2,
  ...str('env'),
  ...str('f'),
  0,
  0,
  ...str('env'),
  ...str('table'),
  1,
  0x70,
  0,
  2,
]);

const tableImport = section(2, [
  1,
  ...str('env'),
  ...str('table'),
  1,
  0x70,
  0,
  2,
]);

function boundedTableImport(maximum) {
  return section(2, [
    1,
    ...str('env'),
    ...str('table'),
    1,
    0x70,
    1,
    2,
    maximum,
  ]);
}

const producer = moduleBytes([
  typeI32,
  importsWithCallbackAndTable,
  section(9, [1, 0, 0x41, 1, 0x0b, 1, 0]),
]);

const table = new WebAssembly.Table({ element: 'anyfunc', initial: 2 });

new WebAssembly.Instance(new WebAssembly.Module(producer), { env: { f: () => 42, table } });

assert.strictEqual(table.get(1)(), 42);

const callback = table.get(1);
table.set(0, callback);
assert.strictEqual(table.get(0), callback);
assert.strictEqual(table.get(0)(), 42);

new WebAssembly.Instance(
  new WebAssembly.Module(moduleBytes([boundedTableImport(4)])),
  { env: { table: new WebAssembly.Table({ element: 'anyfunc', initial: 2, maximum: 2 }) } },
);

assert.throws(
  () => new WebAssembly.Instance(
    new WebAssembly.Module(moduleBytes([boundedTableImport(2)])),
    { env: { table: new WebAssembly.Table({ element: 'anyfunc', initial: 2, maximum: 4 }) } },
  ),
  error => error?.name === 'LinkError',
);
