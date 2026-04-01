export function buildWasm({ types, funcs, exports }) {
  const str = s => [s.length, ...Buffer.from(s)];
  const section = (id, data) => [id, data.length, ...data];

  return new Uint8Array([
    0x00,
    0x61,
    0x73,
    0x6d, // magic
    0x01,
    0x00,
    0x00,
    0x00, // version

    ...section(0x01, [
      // types
      types.length,
      ...types.flatMap(([params, results]) => [0x60, params.length, ...params, results.length, ...results])
    ]),

    ...section(0x03, [
      // funcs -> type index
      funcs.length,
      ...funcs.map((_, i) => i)
    ]),

    ...section(0x07, [
      // exports
      exports.length,
      ...exports.flatMap(([name, kind, idx]) => [...str(name), kind, idx])
    ]),

    ...section(0x0a, [
      funcs.length,
      ...funcs.flatMap(({ locals = [], body }) => {
        const code = [locals.length, ...body, 0x0b];
        return [code.length, ...code];
      })
    ])
  ]);
}
