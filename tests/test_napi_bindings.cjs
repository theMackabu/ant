const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const { pathToFileURL } = require('node:url');

if (process.platform === 'win32') {
  console.log('skip: native addon fixture requires a Unix compiler');
  process.exit(0);
}


async function main(addon) {
  const b = require(addon);

  assert.strictEqual(b.zero(), 0);
  assert.strictEqual(b.nothing(), undefined);
  assert.ok(Object.is(b.identity(0), 0));
  assert.ok(Object.is(b.identity(-0), -0));
  assert.deepStrictEqual(b.callWithZero((...values) => values), [0, 7]);
  assert.strictEqual(b.callWithZero((value) => value), 0);

  assert.throws(() => b.throwCoded(), (e) => e instanceof TypeError && e.code === 'ERR_ANT_TEST' && e.message === 'coded failure');
  const coded = b.createCoded('ERR_ANT_RANGE', 'too far');
  assert.ok(coded instanceof RangeError);
  assert.strictEqual(coded.code, 'ERR_ANT_RANGE');
  assert.strictEqual(coded.message, 'too far');
  assert.strictEqual(b.isError(coded), true);
  assert.strictEqual(b.isError(new Error('x')), true);
  assert.strictEqual(b.isError({ message: 'x' }), false);

  assert.strictEqual(b.latin1(), 'caf\u00e9');
  assert.deepStrictEqual(b.readLatin1('caf\u00e9'), [0x63, 0x61, 0x66, 0xe9]);
  assert.strictEqual(b.readUtf8('\u00e9\u00e9\u00e9', 4), '\u00e9');
  assert.strictEqual(b.readUtf8('\u00e9\u00e9\u00e9', 8), '\u00e9\u00e9\u00e9');

  for (const value of [0, -1, 2 ** 31, 2 ** 32 + 5, -(2 ** 32) - 5, 1.9, -1.9, NaN, Infinity, -Infinity])
    assert.strictEqual(b.toInt32(value), value | 0, `toInt32(${value})`);

  const named = Object.create({ inherited: 1 });
  named.own = 1;
  named[2] = 1;
  Object.defineProperty(named, 'hidden', { value: 1, enumerable: false });
  named[Symbol('s')] = 1;
  assert.deepStrictEqual(b.propertyNames(named).sort(), ['2', 'inherited', 'own']);

  assert.strictEqual(b.hasElement([1, 2], 1), true);
  assert.strictEqual(b.hasElement([1, 2], 2), false);
  const deletable = { gone: 1 };
  assert.strictEqual(b.deleteProperty(deletable, 'gone'), true);
  assert.strictEqual('gone' in deletable, false);
  assert.strictEqual(b.deleteProperty(Object.freeze({ kept: 1 }), 'kept'), false);

  const external = b.externalBuffer();
  assert.ok(Buffer.isBuffer(external));
  assert.deepStrictEqual([...external], [1, 2, 3, 4]);
  external[1] = 77;
  assert.strictEqual(b.externalBufferByte(1), 77);

  assert.deepStrictEqual(b.escape(), { escaped: true });

  const tagged = {};
  assert.strictEqual(b.checkTag(tagged, 0), false);
  assert.strictEqual(b.tag(tagged, 0), true);
  assert.strictEqual(b.checkTag(tagged, 0), true);
  assert.strictEqual(b.checkTag(tagged, 1), false);
  assert.strictEqual(b.tag(tagged, 1), false);
  assert.strictEqual(b.checkTag(tagged, 0), true);
  assert.strictEqual(b.checkTag({}, 0), false);
  const taggedFn = () => {};
  assert.strictEqual(b.tag(taggedFn, 1), true);
  assert.strictEqual(b.checkTag(taggedFn, 1), true);
  assert.strictEqual(b.checkTag(taggedFn, 0), false);

  const frozen = { a: 1 };
  assert.strictEqual(b.freeze(frozen), true);
  assert.strictEqual(Object.isFrozen(frozen), true);
  const sealed = { a: 1 };
  assert.strictEqual(b.seal(sealed), true);
  assert.strictEqual(Object.isSealed(sealed), true);
  assert.strictEqual(Object.isFrozen(sealed), false);

  const proto = { hello() { return 'hi'; } };
  const target = {};
  assert.strictEqual(b.setPrototype(target, proto), true);
  assert.strictEqual(Object.getPrototypeOf(target), proto);
  assert.strictEqual(target.hello(), 'hi');
  assert.strictEqual(b.setPrototype(target, null), true);
  assert.strictEqual(Object.getPrototypeOf(target), null);

  const sym = Symbol('s');
  const made = b.createWith(proto, 'x', 1, sym, 2);
  assert.strictEqual(Object.getPrototypeOf(made), proto);
  assert.strictEqual(made.x, 1);
  assert.strictEqual(made[sym], 2);
  assert.deepStrictEqual(Object.keys(made), ['x']);
  assert.strictEqual(Object.getPrototypeOf(b.createWith(null, 'x', 1, sym, 2)), null);
  let setterCalls = 0;
  const viaSetter = b.createWith({ set x(v) { setterCalls++; } }, 'x', 5, sym, 2);
  assert.strictEqual(setterCalls, 0);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(viaSetter, 'x'), {
    value: 5, writable: true, enumerable: true, configurable: true,
  });

  assert.strictEqual(b.utf16(), 'hé\u{1F600}');
  assert.deepStrictEqual(b.keys(), ['café', 'kéy', 'hé\u{1F600}']);
  for (const [which, expected] of [[0, 'café'], [1, 'hé\u{1F600}']]) {
    const [value, copied, finalized] = b.externalString(which);
    assert.strictEqual(value, expected);
    if (copied) assert.strictEqual(finalized, 1);
  }

  assert.strictEqual(b.symbolFor(), Symbol.for('ant.napi.test'));

  const syntaxError = b.syntaxError('oops');
  assert.ok(syntaxError instanceof SyntaxError);
  assert.strictEqual(syntaxError.message, 'oops');
  assert.throws(() => b.throwSyntax(), (e) => e instanceof SyntaxError && e.message === 'bad syntax');

  const viewed = new ArrayBuffer(8);
  new Uint8Array(viewed)[2] = 42;
  const view = b.dataView(viewed, 2, 4);
  assert.ok(view instanceof DataView);
  assert.strictEqual(view.byteLength, 4);
  assert.strictEqual(view.byteOffset, 2);
  assert.strictEqual(view.buffer, viewed);
  assert.strictEqual(view.getUint8(0), 42);
  assert.deepStrictEqual(b.dataViewInfo(view), [4, 2, viewed, 42]);
  assert.throws(() => b.dataView(viewed, 6, 4), RangeError);

  const detachable = new ArrayBuffer(4);
  assert.strictEqual(b.isDetached(detachable), false);
  assert.strictEqual(b.detach(detachable), true);
  assert.strictEqual(b.isDetached(detachable), true);
  assert.strictEqual(detachable.byteLength, 0);
  assert.strictEqual(b.isDetached({}), false);
  assert.strictEqual(b.detach({}), false);

  const backing = new ArrayBuffer(8);
  const bytes = new Uint8Array(backing);
  for (let i = 0; i < 8; i++) bytes[i] = i;
  const buffer = b.bufferFrom(backing, 2, 3);
  assert.ok(Buffer.isBuffer(buffer));
  assert.deepStrictEqual([...buffer], [2, 3, 4]);
  assert.strictEqual(buffer.buffer, backing);
  assert.strictEqual(buffer.byteOffset, 2);
  buffer[0] = 99;
  assert.strictEqual(bytes[2], 99);
  assert.throws(() => b.bufferFrom(backing, 6, 4), RangeError);

  const shared = b.sharedBuffer(4);
  assert.ok(shared instanceof SharedArrayBuffer);
  assert.deepStrictEqual([...new Uint8Array(shared)], [1, 2, 3, 4]);
  assert.strictEqual(b.isShared(shared), true);
  assert.strictEqual(b.isShared(new ArrayBuffer(1)), false);
  assert.strictEqual(b.isShared({}), false);
  assert.strictEqual(b.detach(shared), false);

  const externalShared = b.externalSharedBuffer();
  assert.ok(externalShared instanceof SharedArrayBuffer);
  assert.deepStrictEqual([...new Uint8Array(externalShared)], [9, 8, 7, 6]);
  assert.strictEqual(b.isShared(externalShared), true);

  assert.strictEqual(b.moduleFileName(), pathToFileURL(addon).href);

  assert.strictEqual(b.postFinalizer(), true);
  assert.strictEqual(b.posted(), false);
  await new Promise((resolve) => setTimeout(resolve, 50));
  assert.strictEqual(b.posted(), true);

  assert.strictEqual(b.addRemoveAsyncCleanup(), true);
  const child = spawnSync(process.execPath, [__filename, 'child', addon], { encoding: 'utf8', timeout: 30000 });
  assert.strictEqual(child.status, 0, child.stderr);
  assert.strictEqual(child.stdout, 'async cleanup hook ran\n');
}

function run() {
  const directory = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-napi-bindings-')));
  const addon = path.join(directory, 'binding.node');
  const source = path.join(__dirname, 'fixtures/napi-bindings/binding.c');
  const flags = process.platform === 'darwin' ? ['-bundle', '-undefined', 'dynamic_lookup'] : ['-shared', '-fPIC'];
  const build = spawnSync('cc', [...flags, source, '-o', addon], { encoding: 'utf8', timeout: 30000 });
  assert.strictEqual(build.status, 0, build.stderr);

  main(addon).then(
    () => {
      fs.rmSync(directory, { recursive: true, force: true });
      console.log('N-API bindings ok');
    },
    (error) => {
      fs.rmSync(directory, { recursive: true, force: true });
      console.error(error);
      process.exit(1);
    }
  );
}

if (process.argv[2] === 'child') assert.strictEqual(require(process.argv[3]).installAsyncCleanup(), true);
else run();
