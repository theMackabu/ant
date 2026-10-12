const util = require('util');

function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

process.env.ANT_ENV_PROXY_T = 7;
assert(process.env.ANT_ENV_PROXY_T === '7', 'values are coerced to strings');
assert('ANT_ENV_PROXY_T' in process.env && 'HOME' in process.env, 'in sees set and inherited vars');
assert(!('ANT_ENV_PROXY_MISSING' in process.env), 'in rejects missing vars');

const keys = Object.keys(process.env);
assert(keys.includes('ANT_ENV_PROXY_T') && keys.includes('HOME'), 'keys list set and inherited vars');

const spread = { ...process.env };
assert(spread.ANT_ENV_PROXY_T === '7' && typeof spread.HOME === 'string', 'spread copies the environment');
assert(Object.assign({}, process.env).ANT_ENV_PROXY_T === '7', 'Object.assign copies the environment');

let seen = 0;
for (const k in process.env) if (k === 'HOME' || k === 'ANT_ENV_PROXY_T') seen++;
assert(seen === 2, 'for-in visits set and inherited vars');

const desc = Object.getOwnPropertyDescriptor(process.env, 'HOME');
assert(desc && desc.enumerable && desc.configurable && desc.writable, 'inherited vars have data descriptors');

assert(!util.types.isProxy(process.env), 'process.env is not reported as a proxy');
assert(util.inspect(process.env).includes('ANT_ENV_PROXY_T'), 'inspect shows the environment');

delete process.env.ANT_ENV_PROXY_T;
assert(process.env.ANT_ENV_PROXY_T === undefined && !('ANT_ENV_PROXY_T' in process.env), 'delete removes the var');

process.env.ANT_ENV_PROXY_CHILD = 'inherited';
const cp = require('child_process');
const child = cp.spawnSync('/bin/sh', ['-c', 'printf "%s|%s" "$HOME" "$ANT_ENV_PROXY_CHILD"'], { env: process.env });
assert(String(child.stdout) === process.env.HOME + '|inherited', 'spawnSync with env: process.env passes the environment');
const execOut = cp.execSync('printf "%s" "$ANT_ENV_PROXY_CHILD"', { env: process.env });
assert(String(execOut) === 'inherited', 'execSync with env: process.env passes the environment');

assert(Object.getOwnPropertyNames(process.env).includes('ANT_ENV_PROXY_CHILD'), 'getOwnPropertyNames lists env vars');
assert(Reflect.ownKeys(process.env).includes('HOME'), 'Reflect.ownKeys lists env vars');
assert(Object.getOwnPropertyDescriptors(process.env).HOME.value === process.env.HOME, 'getOwnPropertyDescriptors sees env vars');
assert(structuredClone(process.env).ANT_ENV_PROXY_CHILD === 'inherited', 'structuredClone copies env vars');

let setErr;
try { process.env.ANT_ENV_PROXY_BAD = { toString() { throw new Error('bad-tostring'); } }; }
catch (e) { setErr = e; }
assert(setErr && setErr.message === 'bad-tostring', 'a failing set reports the original error');

let badNameErr;
try { process.env['ANT_ENV=BAD'] = 'x'; }
catch (e) { badNameErr = e; }
assert(badNameErr instanceof TypeError, 'an invalid variable name throws');

Object.defineProperty(process.env, 'ANT_ENV_PROXY_DEF', { value: 5 });
assert(process.env.ANT_ENV_PROXY_DEF === '5', 'defineProperty stores the value in the environment');
let accessorErr;
try { Object.defineProperty(process.env, 'ANT_ENV_PROXY_ACC', { get() { return 1; } }); }
catch (e) { accessorErr = e; }
assert(accessorErr instanceof TypeError, 'accessors cannot be defined on env vars');

delete process.env.ANT_ENV_PROXY_CHILD;
delete process.env.ANT_ENV_PROXY_DEF;

const p = new Proxy({}, {
  ownKeys: () => ['a', 'b'],
  getOwnPropertyDescriptor: () => ({ value: 0, enumerable: true, configurable: true }),
  get: (t, k) => 'v' + String(k),
});
const copy = { ...p };
assert(copy.a === 'va' && copy.b === 'vb', 'object spread uses proxy traps');
assert(util.types.isProxy(p), 'user proxies are still proxies');
assert(Reflect.ownKeys(p).join() === 'a,b' && Object.getOwnPropertyNames(p).join() === 'a,b', 'key APIs use the ownKeys trap');

const sym = Symbol('s');
const withSym = new Proxy({ x: 1, [sym]: 2 }, {});
const symCopy = { ...withSym };
assert(symCopy.x === 1 && symCopy[sym] === 2, 'object spread copies symbol keys from proxies');

function throws(fn) {
  try { fn(); } catch (e) { return e; }
  return null;
}

const throwing = new Proxy({}, {
  ownKeys: () => ['a'],
  getOwnPropertyDescriptor: () => ({ value: 0, enumerable: true, configurable: true }),
  get() { throw new Error('boom-get'); },
});
assert(throws(() => ({ ...throwing }))?.message === 'boom-get', 'spread rethrows a trap error');
assert(throws(() => { const { ...rest } = throwing; return rest; })?.message === 'boom-get', 'object rest rethrows a trap error');

const { proxy: revocable, revoke } = Proxy.revocable({ a: 1 }, {});
revoke();
assert(throws(() => ({ ...revocable })) instanceof TypeError, 'spreading a revoked proxy throws');

const getterThrows = { get a() { throw new Error('boom-accessor'); } };
assert(throws(() => ({ ...getterThrows }))?.message === 'boom-accessor', 'spread rethrows getter errors');

Object.freeze(process.env);
process.env.ANT_ENV_PROXY_AFTER_FREEZE = 'x';
assert(process.env.ANT_ENV_PROXY_AFTER_FREEZE === 'x', 'freezing process.env leaves the environment writable');
delete process.env.ANT_ENV_PROXY_AFTER_FREEZE;

console.log('process.env proxy OK');
