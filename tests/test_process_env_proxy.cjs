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

function throws(fn) {
  try { fn(); } catch (e) { return e; }
  return null;
}

// like node and bun, a name the environment cannot hold is accepted and dropped
process.env['ANT_ENV=BAD'] = 'x';
assert(Reflect.set(process.env, '', 'x') === true && !('' in process.env), 'an empty name is accepted and dropped');
assert(throws(() => { 'use strict'; process.env['ANT_ENV=BAD'] = 'x'; }) === null, 'a strict write to an invalid name does not throw');

process.env.ANT_ENV_PROXY_EQ = '1';
assert(Reflect.set(process.env, 'ANT_ENV_PROXY_EQ=Q', '2') === true, "a name with '=' is accepted");
assert(process.env.ANT_ENV_PROXY_EQ === '1', "a name with '=' never reaches the var before it");
delete process.env.ANT_ENV_PROXY_EQ;

process.env.ANT_ENV_PROXY_NUL = 'kept';
assert(Reflect.set(process.env, 'ANT_ENV_PROXY_NUL\0X', 'clobbered') === true, 'a name with NUL is accepted');
assert(process.env['ANT_ENV_PROXY_NUL\0X'] === undefined, 'a name with NUL reads as absent');
assert(delete process.env['ANT_ENV_PROXY_NUL\0X'] && process.env.ANT_ENV_PROXY_NUL === 'kept', 'a name with NUL never reaches the var before it');
delete process.env.ANT_ENV_PROXY_NUL;

const inheritsEnv = Object.create(process.env);
inheritsEnv.ANT_ENV_PROXY_RECV = 'r';
assert(Object.hasOwn(inheritsEnv, 'ANT_ENV_PROXY_RECV'), 'a write through the prototype chain lands on the receiver');
assert(process.env.ANT_ENV_PROXY_RECV === undefined, 'a write through the prototype chain leaves the environment alone');

const full = { value: 5, writable: true, enumerable: true, configurable: true };
Object.defineProperty(process.env, 'ANT_ENV_PROXY_DEF', full);
assert(process.env.ANT_ENV_PROXY_DEF === '5', 'a full data descriptor stores the value in the environment');
assert(throws(() => Object.defineProperty(process.env, 'ANT_ENV_PROXY_DEF', { value: 'x' })) instanceof TypeError, 'a partial descriptor is rejected');
assert(throws(() => Object.defineProperty(process.env, 'ANT_ENV_PROXY_ACC', { get() { return 1; } })) instanceof TypeError, 'accessors cannot be defined on env vars');
assert(process.env.ANT_ENV_PROXY_DEF === '5', 'a rejected descriptor changes nothing');

const envSym = Symbol('env');
const symbolError = throws(() => { process.env[envSym] = 1; });
assert(symbolError instanceof TypeError && symbolError.message === 'Cannot convert a Symbol value to a string', 'a symbol key cannot be set');
assert(throws(() => Object.defineProperty(process.env, envSym, full)) instanceof TypeError, 'a symbol key cannot be defined');
assert(throws(() => { process.env.ANT_ENV_PROXY_SYMV = Symbol('v'); }) instanceof TypeError, 'a symbol value cannot be stored');
assert(process.env[envSym] === undefined && !(envSym in process.env) && delete process.env[envSym], 'a symbol key reads as absent');
assert(Object.getOwnPropertyNames(process.env).includes('toObject'), 'own methods of process.env are listed');
assert(!Object.keys(process.env).includes('toObject'), 'own methods of process.env are not enumerable');

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

const log = [];
const traced = new Proxy({ a: 1, b: 2, c: 3 }, {
  ownKeys: (t) => { log.push('ownKeys'); return Reflect.ownKeys(t); },
  getOwnPropertyDescriptor: (t, k) => { log.push('desc:' + k); return Reflect.getOwnPropertyDescriptor(t, k); },
  get: (t, k) => { log.push('get:' + String(k)); return t[k]; },
});
const { a: tracedA, ...tracedRest } = traced;
assert(tracedA === 1 && tracedRest.b === 2 && tracedRest.c === 3 && !('a' in tracedRest), 'rest leaves out named keys');
assert(log.join() === 'get:a,ownKeys,desc:b,get:b,desc:c,get:c', 'rest skips excluded keys and reads each key before the next: ' + log.join());

let keyCalls = 0;
const key = () => { keyCalls++; return 'b'; };
const { [key()]: computedB, ...afterComputed } = { a: 1, b: 2 };
assert(keyCalls === 1 && computedB === 2 && !('b' in afterComputed) && afterComputed.a === 1, 'a computed rest key is evaluated once');

let excludedGetterCalls = 0;
const { x: namedX, ...noX } = { get x() { excludedGetterCalls++; return 1; }, y: 2 };
assert(excludedGetterCalls === 1 && noX.y === 2 && !('x' in noX), 'an excluded getter runs only for its own binding');

const withProtoKey = JSON.parse('{"__proto__": {"polluted": true}, "z": 1}');
const protoCopy = { ...withProtoKey };
assert(Object.getPrototypeOf(protoCopy) === Object.prototype && Object.hasOwn(protoCopy, '__proto__'), 'spread defines __proto__ as an own key');

assert(throws(() => Object.freeze(process.env))?.message === 'Cannot freeze', 'freezing process.env throws');
assert(throws(() => Object.seal(process.env))?.message === 'Cannot seal', 'sealing process.env throws');
assert(throws(() => Object.preventExtensions(process.env))?.message === 'Cannot prevent extensions', 'preventExtensions on process.env throws');
assert(Reflect.preventExtensions(process.env) === false, 'Reflect.preventExtensions on process.env returns false');
assert(Object.isExtensible(process.env) && !Object.isFrozen(process.env), 'process.env stays extensible');

const fs = require('fs');
const os = require('os');
const path = require('path');
const dotenvDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-dotenv-'));
fs.writeFileSync(path.join(dotenvDir, '.env'), 'ANT_DOTENV_BOTH=file\nANT_DOTENV_FILE=file\n');
const dotenvChild = cp.spawnSync(process.execPath, ['-e', 'console.log(process.env.ANT_DOTENV_BOTH + "|" + process.env.ANT_DOTENV_FILE)'], {
  cwd: dotenvDir,
  env: { ...process.env, ANT_DOTENV_BOTH: 'env' },
});
fs.rmSync(dotenvDir, { recursive: true, force: true });
assert(String(dotenvChild.stdout).trim() === 'env|file', '.env fills gaps but the real environment wins: ' + String(dotenvChild.stdout).trim());

console.log('process.env proxy OK');
