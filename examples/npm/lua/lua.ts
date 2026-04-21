import assert from 'node:assert';
import { LuaJS, LuaFunction, LuaTable } from '@doridian/luajs';

async function test(name: string, fn: () => Promise<void> | void) {
  try {
    await fn();
    console.log(`✓ ${name}`);
  } catch (err) {
    console.error(`✗ ${name}`);
    console.error(err);
  }
}

function convertBack(ret: unknown[]): [string, unknown] {
  return [ret[0] as string, (ret[1] as LuaTable).toObject(true, true)];
}

await test('Can run basic Lua code', async () => {
  const L = await LuaJS.newState();
  const ret = await L.run('return 1 + 2, "hello"');
  assert.deepEqual(ret, [3, 'hello']);
});

await test('Can pass JS types to Lua correctly', async () => {
  const L = await LuaJS.newState();
  const ret = await L.run('return function(a) return type(a), a end');
  const retConvert = await L.run('return function(a) a = a:toTable(true, 10); return type(a), a end');

  const func = (ret[0] as LuaFunction).getClosure();
  const funcConvert = (retConvert[0] as LuaFunction).getClosure();

  assert.deepEqual(await func('hello world'), ['string', 'hello world']);
  assert.deepEqual(await func(13), ['number', 13]);
  assert.deepEqual(await func([1, 2, 3]), ['userdata', [1, 2, 3]]);

  assert.deepEqual(convertBack(await funcConvert([1, 2, true, undefined, null, , 3])), ['table', [1, 2, true, , , , 3]]);
  assert.deepEqual(convertBack(await funcConvert({ a: 1, b: '2', c: true, d: undefined, e: null })), ['table', { a: 1, b: '2', c: true }]);

  assert.deepEqual(convertBack(await funcConvert({ a: 1, b: [4, { x: 5, y: [6, 9, 42] }, 6], c: true })), [
    'table',
    { a: 1, b: [4, { x: 5, y: [6, 9, 42] }, 6], c: true }
  ]);
});

await test('allows awaiting JS promises', async () => {
  const L = await LuaJS.newState();
  await L.run('return js.await');
});
