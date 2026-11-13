const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const sqlite = require('node:sqlite');

const { Database, DatabaseSync, Statement, StatementSync, Session, constants, backup } = sqlite;
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-sqlite-'));
function plain(value) {
  if (Array.isArray(value)) return value.map(plain);
  if (value === null || typeof value !== 'object' || Object.getPrototypeOf(value) !== null) return value;
  return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, plain(item)]));
}

function testExports() {
  assert.strictEqual(DatabaseSync, Database);
  assert.strictEqual(StatementSync, Statement);
  assert.strictEqual(typeof Session, 'function');
  assert.strictEqual(typeof backup, 'function');
  assert.strictEqual(Object.keys(constants).length, 45);
  assert.strictEqual(constants.SQLITE_CHANGESET_ABORT, 2);
  assert.strictEqual(constants.SQLITE_RECURSIVE, 33);

  assert.throws(() => Database(':memory:'), { code: 'ERR_CONSTRUCT_CALL_REQUIRED' });
  assert.throws(() => new Statement(), { code: 'ERR_ILLEGAL_CONSTRUCTOR' });
  assert.throws(() => new Session(), { code: 'ERR_ILLEGAL_CONSTRUCTOR' });
  assert.throws(() => new Database(1), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => new Database(':memory:', { open: 1 }), { code: 'ERR_INVALID_ARG_TYPE', message: /options\.open/ });
  assert.throws(() => new Database(':memory:', { readOnly: 'x' }), { code: 'ERR_INVALID_ARG_TYPE' });
}

function testOpenClose() {
  const db = new Database(':memory:', { open: false });
  assert.strictEqual(db.isOpen, false);
  assert.throws(() => db.exec('select 1'), { code: 'ERR_INVALID_STATE', message: /not open/ });
  db.open();
  assert.strictEqual(db.isOpen, true);
  assert.throws(() => db.open(), { code: 'ERR_INVALID_STATE', message: /already open/ });
  assert.strictEqual(db.location(), null);
  assert.strictEqual(db.isTransaction, false);
  db.exec('begin');
  assert.strictEqual(db.isTransaction, true);
  db.exec('rollback');

  const statement = db.prepare('select 1 as one');
  db.close();
  assert.strictEqual(db.isOpen, false);
  assert.throws(() => db.close(), { code: 'ERR_INVALID_STATE' });
  assert.throws(() => statement.get(), { code: 'ERR_INVALID_STATE', message: /finalized/ });

  const disposable = new Database(':memory:');
  disposable[Symbol.dispose]();
  assert.strictEqual(disposable.isOpen, false);
  disposable[Symbol.dispose]();
}

function testFiles() {
  const file = path.join(dir, 'file.db');
  const db = new Database(file);
  db.exec('create table t(a); insert into t values (1)');
  assert.strictEqual(fs.realpathSync(db.location()), fs.realpathSync(file));
  db.close();

  const fromUrl = new Database(pathToFileURL(file));
  assert.strictEqual(fromUrl.prepare('select a from t').get().a, 1);
  fromUrl.close();

  const fromBuffer = new Database(Buffer.from(file));
  assert.strictEqual(fromBuffer.prepare('select a from t').get().a, 1);
  fromBuffer.close();

  const readOnly = new Database(file, { readOnly: true });
  assert.throws(() => readOnly.exec('insert into t values (2)'), { code: 'ERR_SQLITE_ERROR' });
  readOnly.close();

  assert.throws(() => new Database(path.join(dir, 'missing', 'x.db')), { code: 'ERR_SQLITE_ERROR' });
}

function testBindingAndReading() {
  const db = new Database(':memory:');
  db.exec('create table t(id integer primary key, name text, score real, data blob)');

  const insert = db.prepare('insert into t(name, score, data) values (?, ?, ?)');
  assert.deepStrictEqual(insert.run('a', 1.5, new Uint8Array([1, 2, 3])), { changes: 1, lastInsertRowid: 1 });
  assert.deepStrictEqual(insert.run('b', 2, null), { changes: 1, lastInsertRowid: 2 });
  assert.strictEqual(insert.sourceSQL, 'insert into t(name, score, data) values (?, ?, ?)');
  assert.strictEqual(insert.expandedSQL, "insert into t(name, score, data) values ('b', 2.0, NULL)");

  const row = db.prepare('select * from t where id = ?').get(1);
  assert.strictEqual(Object.getPrototypeOf(row), null);
  assert.deepStrictEqual(plain(row), { id: 1, name: 'a', score: 1.5, data: new Uint8Array([1, 2, 3]) });
  assert.strictEqual(db.prepare('select * from t where id = ?').get(99), undefined);
  assert.deepStrictEqual(db.prepare('select id from t order by id').all().map(r => r.id), [1, 2]);

  const types = db.prepare('select ? as a, ? as b, ? as c, ? as d, ? as e, ? as f, typeof(?) as g, typeof(?) as h');
  const buffer = new Uint8Array([7, 8]).buffer;
  const result = types.get(true, false, undefined, 12n, new DataView(buffer), buffer, 1, 2n);
  assert.deepStrictEqual(plain(result), {
    a: 1, b: 0, c: null, d: 12,
    e: new Uint8Array([7, 8]), f: new Uint8Array([7, 8]),
    g: 'real', h: 'integer'
  });

  assert.deepStrictEqual(plain(db.prepare('select ?2 as a, ?1 as b').get('first', 'second')), { a: 'second', b: 'first' });
  assert.deepStrictEqual(plain(db.prepare('select $a as a, $b as b').get({ $a: 1, $b: 2 })), { a: 1, b: 2 });
  assert.deepStrictEqual(plain(db.prepare('select :a as a').get({ ':a': 1 })), { a: 1 });
  assert.deepStrictEqual(plain(db.prepare('select @a as a').get({ '@a': 1 })), { a: 1 });
  assert.deepStrictEqual(plain(db.prepare('select $k as a, $k as b').get({ k: 7 })), { a: 7, b: 7 });
  assert.deepStrictEqual(plain(db.prepare('select $a as a, ? as b').get({ a: 1 }, 2)), { a: 1, b: 2 });
  assert.deepStrictEqual(plain(db.prepare('select $a as a').get({ $a: undefined })), { a: null });

  assert.throws(() => db.prepare('select $a').get({ b: 1 }), { code: 'ERR_INVALID_STATE', message: "Unknown named parameter 'b'" });
  assert.throws(() => db.prepare('select :k, @k').get({ k: 1 }), { code: 'ERR_INVALID_STATE', message: /conflicting names ':k' and '@k'/ });
  assert.throws(() => db.prepare('select ?').get(Symbol('x')), { code: 'ERR_INVALID_ARG_TYPE', message: /parameter 1/ });
  assert.throws(() => db.prepare('select ?').get(2n ** 70n), { code: 'ERR_INVALID_ARG_VALUE' });
  assert.throws(() => db.prepare('select ?').get(1, 2), { code: 'ERR_SQLITE_ERROR', errcode: 25 });
  assert.throws(() => db.prepare(''), { code: 'ERR_INVALID_ARG_VALUE' });
  assert.throws(() => db.prepare(1), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => db.exec('select * from nope'), { code: 'ERR_SQLITE_ERROR', errcode: 1, errstr: 'SQL logic error', message: 'no such table: nope' });

  const unknown = db.prepare('select $a as a');
  unknown.setAllowUnknownNamedParameters(true);
  assert.deepStrictEqual(plain(unknown.get({ a: 1, zzz: 2 })), { a: 1 });

  const bare = db.prepare('select $a as a');
  bare.setAllowBareNamedParameters(false);
  assert.throws(() => bare.get({ a: 1 }), { code: 'ERR_INVALID_STATE' });
  assert.deepStrictEqual(plain(bare.get({ $a: 1 })), { a: 1 });

  const big = db.prepare('select 9223372036854775807 as big');
  assert.throws(() => big.get(), { code: 'ERR_OUT_OF_RANGE' });
  big.setReadBigInts(true);
  assert.strictEqual(big.get().big, 9223372036854775807n);
  assert.deepStrictEqual(db.prepare('insert into t(name) values (?)', { readBigInts: true }).run('c'), { changes: 1n, lastInsertRowid: 3n });

  const arrays = db.prepare('select 1 as a, 2 as b');
  arrays.setReturnArrays(true);
  assert.deepStrictEqual(arrays.get(), [1, 2]);
  assert.deepStrictEqual(arrays.all(), [[1, 2]]);
  assert.deepStrictEqual(db.prepare('select 3 as a', { returnArrays: true }).get(), [3]);

  const configured = new Database(':memory:', { readBigInts: true, returnArrays: true, allowBareNamedParameters: false, allowUnknownNamedParameters: true });
  assert.deepStrictEqual(configured.prepare('select 1, $a').get({ $a: 2, extra: 3 }), [1n, 2]);
  assert.deepStrictEqual(configured.prepare('select $a').get({ a: 2 }), [null]);
  configured.close();

  assert.throws(() => arrays.setReturnArrays(1), { code: 'ERR_INVALID_ARG_TYPE' });
  db.close();
}

function testColumnsStatsAndClose() {
  const db = new Database(':memory:');
  db.exec('create table t(a integer primary key, b text); insert into t(b) values (\'x\'), (\'y\')');

  const statement = db.prepare('select a as z, b, a + 1 as c from t');
  assert.deepStrictEqual(statement.columns().map(plain), [
    { column: 'a', database: 'main', name: 'z', table: 't', type: 'INTEGER' },
    { column: 'b', database: 'main', name: 'b', table: 't', type: 'TEXT' },
    { column: null, database: null, name: 'c', table: null, type: null }
  ]);

  statement.all();
  assert.strictEqual(statement.stat('run'), 1);
  assert.ok(statement.stat('vmStep') > 0);
  assert.ok(statement.stat('fullscanStep') >= 1);
  assert.ok(statement.stat('memused') > 0);
  statement.resetStats();
  assert.strictEqual(statement.stat('run'), 0);
  assert.strictEqual(statement.stat('vmStep'), 0);
  assert.throws(() => statement.stat('nope'), { code: 'ERR_INVALID_ARG_VALUE' });
  assert.throws(() => statement.stat(1), { code: 'ERR_INVALID_ARG_TYPE' });

  statement.close();
  assert.throws(() => statement.close(), { code: 'ERR_INVALID_STATE' });
  assert.throws(() => statement.all(), { code: 'ERR_INVALID_STATE' });
  statement[Symbol.dispose]();

  const disposable = db.prepare('select 1');
  disposable[Symbol.dispose]();
  assert.throws(() => disposable.get(), { code: 'ERR_INVALID_STATE' });
  db.close();
}

function testIterate() {
  const db = new Database(':memory:');
  db.exec('create table t(a); insert into t values (1), (2), (3)');
  const statement = db.prepare('select a from t where a >= ? order by a');

  const iterator = statement.iterate(2);
  assert.strictEqual(Object.getPrototypeOf(Object.getPrototypeOf(iterator)), Iterator.prototype);
  assert.deepStrictEqual(plain(iterator.next()), { done: false, value: { a: 2 } });
  assert.deepStrictEqual([...iterator].map(plain), [{ a: 3 }]);
  assert.deepStrictEqual(plain(iterator.next()), { done: true, value: null });
  assert.deepStrictEqual(plain(iterator.return()), { done: true, value: null });

  assert.deepStrictEqual([...statement.iterate(1)].map(r => r.a), [1, 2, 3]);
  assert.deepStrictEqual(statement.iterate(1).map(r => r.a * 2).toArray(), [2, 4, 6]);

  const early = statement.iterate(1);
  early.next();
  assert.deepStrictEqual(plain(early.return()), { done: true, value: null });
  assert.deepStrictEqual(plain(early.next()), { done: true, value: null });

  const stale = statement.iterate(1);
  stale.next();
  statement.all(1);
  assert.throws(() => stale.next(), { code: 'ERR_INVALID_STATE', message: /invalidated/ });

  assert.deepStrictEqual([...db.prepare('select a from t where a > 5').iterate()], []);
  db.close();
}

function testFunctions() {
  const db = new Database(':memory:');

  db.function('plus', (a, b) => a + b);
  assert.strictEqual(db.prepare('select plus(1, 2) as v').get().v, 3);
  assert.throws(() => db.prepare('select plus(1)'), { code: 'ERR_SQLITE_ERROR' });

  db.function('count_args', { varargs: true }, (...args) => args.length);
  assert.strictEqual(db.prepare('select count_args(1, 2, 3, 4) as v').get().v, 4);
  assert.strictEqual(db.prepare('select count_args() as v').get().v, 0);

  db.function('kind', { useBigIntArguments: true }, value => typeof value);
  assert.strictEqual(db.prepare('select kind(5) as v').get().v, 'bigint');

  db.function('values_of', { varargs: true }, (...args) => JSON.stringify(args.map(a => (a instanceof Uint8Array ? [...a] : a))));
  assert.strictEqual(db.prepare("select values_of(1, 1.5, 'x', null, x'0102') as v").get().v, '[1,1.5,"x",null,[1,2]]');

  db.function('void_value', () => undefined);
  db.function('bytes', () => new Uint8Array([9]));
  db.function('wide', () => 12n);
  assert.deepStrictEqual(plain(db.prepare('select void_value() as a, bytes() as b, wide() as d').get()), {
    a: null, b: new Uint8Array([9]), d: 12
  });

  const boom = new Error('boom');
  db.function('explode', () => { throw boom; });
  assert.throws(() => db.prepare('select explode()').get(), error => error === boom);
  assert.strictEqual(db.prepare('select plus(2, 3) as v').get().v, 5);

  db.function('bad', () => ({}));
  assert.throws(() => db.prepare('select bad()').get(), { code: 'ERR_SQLITE_ERROR', message: /cannot be converted/ });
  db.function('flag', () => true);
  assert.throws(() => db.prepare('select flag()').get(), { code: 'ERR_SQLITE_ERROR', message: /cannot be converted/ });
  db.function('huge', () => 2n ** 70n);
  assert.throws(() => db.prepare('select huge()').get(), { code: 'ERR_SQLITE_ERROR', message: /too large/ });
  db.function('later', () => Promise.resolve(1));
  assert.throws(() => db.prepare('select later()').get(), { code: 'ERR_SQLITE_ERROR', message: /Asynchronous/ });

  db.function('det', { deterministic: true, directOnly: true }, () => 1);
  assert.strictEqual(db.prepare('select det() as v').get().v, 1);

  assert.throws(() => db.function('x', {}, 1), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => db.function(1, () => 1), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => db.function('x', { deterministic: 1 }, () => 1), { code: 'ERR_INVALID_ARG_TYPE' });

  let reentrant;
  db.function('reenter', () => {
    assert.throws(() => reentrant.get(), { code: 'ERR_INVALID_STATE' });
    assert.throws(() => reentrant.close(), { code: 'ERR_INVALID_STATE' });
    assert.throws(() => db.close(), { code: 'ERR_INVALID_STATE' });
    return db.prepare('select 41 + 1 as v').get().v;
  });
  reentrant = db.prepare('select reenter() as v');
  assert.strictEqual(reentrant.get().v, 42);
  db.close();
}

function testAggregates() {
  const db = new Database(':memory:');
  db.exec("create table t(x, y); insert into t values ('a', 4), ('b', 5), ('c', 3), ('d', 8), ('e', 1)");

  db.aggregate('sumint', { start: 0, step: (acc, value) => acc + value });
  assert.strictEqual(db.prepare('select sumint(y) as total from t').get().total, 21);
  assert.strictEqual(db.prepare('select sumint(y) as total from t where y > 100').get().total, 0);

  db.aggregate('collect', {
    start: () => [],
    step: (acc, value) => [...acc, value],
    result: acc => acc.join(',')
  });
  assert.strictEqual(db.prepare('select collect(x) as v from (select x from t order by x)').get().v, 'a,b,c,d,e');
  assert.deepStrictEqual(
    db.prepare('select y % 2 as odd, sumint(y) as total from t group by odd order by odd').all().map(plain),
    [{ odd: 0, total: 12 }, { odd: 1, total: 9 }]
  );

  db.aggregate('winsum', {
    start: 0,
    step: (acc, value) => acc + value,
    inverse: (acc, value) => acc - value,
    result: acc => acc
  });
  assert.deepStrictEqual(
    db.prepare('select winsum(y) over (order by x rows between 1 preceding and current row) as v from t order by x').all().map(r => r.v),
    [4, 9, 8, 11, 9]
  );

  db.aggregate('bigsum', { start: 0n, useBigIntArguments: true, step: (acc, value) => acc + value });
  assert.strictEqual(db.prepare('select bigsum(y) as v from t').get().v, 21);

  db.aggregate('varsum', { start: 0, varargs: true, step: (acc, ...values) => acc + values.length });
  assert.strictEqual(db.prepare('select varsum(x, y, 1) as v from t').get().v, 15);

  const boom = new Error('step failed');
  db.aggregate('broken', { start: 0, step: (acc, value) => { throw boom; } });
  assert.throws(() => db.prepare('select broken(y) from t').get(), error => error === boom);

  assert.throws(() => db.aggregate('x', { start: 0 }), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => db.aggregate('x', { step: () => 1 }), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => db.aggregate('x'), { code: 'ERR_INVALID_ARG_TYPE' });
  db.close();
}

function testAuthorizer() {
  const db = new Database(':memory:');
  const seen = [];

  db.setAuthorizer((action, arg1, arg2, dbName, trigger) => {
    seen.push([action, arg1, arg2, dbName, trigger]);
    return action === constants.SQLITE_CREATE_TABLE && arg1 === 'blocked' ? constants.SQLITE_DENY : constants.SQLITE_OK;
  });

  db.exec('create table allowed(id integer)');
  assert.ok(seen.some(entry => entry[0] === constants.SQLITE_CREATE_TABLE && entry[1] === 'allowed' && entry[3] === 'main'));
  assert.throws(() => db.exec('create table blocked(id integer)'), { code: 'ERR_SQLITE_ERROR', message: /not authorized/ });

  db.setAuthorizer(() => {
    assert.throws(() => db.exec('select 1'), { code: 'ERR_INVALID_STATE' });
    assert.throws(() => db.prepare('select 1'), { code: 'ERR_INVALID_STATE' });
    assert.throws(() => db.setAuthorizer(null), { code: 'ERR_INVALID_STATE' });
    return constants.SQLITE_OK;
  });
  db.prepare('select 1').get();

  const boom = new Error('auth failed');
  db.setAuthorizer(() => { throw boom; });
  assert.throws(() => db.prepare('select 1'), error => error === boom);

  db.setAuthorizer(() => 'nope');
  assert.throws(() => db.prepare('select 1'), TypeError);
  db.setAuthorizer(() => 99);
  assert.throws(() => db.prepare('select 1'), RangeError);

  db.setAuthorizer(null);
  assert.strictEqual(db.prepare('select 1 as v').get().v, 1);
  assert.throws(() => db.setAuthorizer(1), { code: 'ERR_INVALID_ARG_TYPE' });
  db.close();
}

function testLimits() {
  const db = new Database(':memory:', { limits: { column: 5, attach: 2 } });
  assert.strictEqual(db.limits, db.limits);
  assert.strictEqual(db.limits.column, 5);
  assert.strictEqual(db.limits.attach, 2);
  assert.deepStrictEqual(Object.keys(db.limits), [
    'length', 'sqlLength', 'column', 'exprDepth', 'compoundSelect', 'vdbeOp',
    'functionArg', 'attach', 'likePatternLength', 'variableNumber', 'triggerDepth'
  ]);

  assert.throws(() => db.exec('create table wide(a, b, c, d, e, f)'), { code: 'ERR_SQLITE_ERROR' });
  db.limits.column = 10;
  assert.strictEqual(db.limits.column, 10);
  db.exec('create table wide(a, b, c, d, e, f)');

  db.limits.sqlLength = 100;
  assert.throws(() => db.exec(`select '${'x'.repeat(200)}'`), { code: 'ERR_SQLITE_ERROR' });
  db.limits.sqlLength = Infinity;
  assert.ok(db.limits.sqlLength >= 1000000);

  assert.throws(() => { db.limits.column = -1; }, { code: 'ERR_OUT_OF_RANGE' });
  assert.throws(() => new Database(':memory:', { limits: { column: -1 } }), { code: 'ERR_OUT_OF_RANGE' });
  db.close();
}

function testSerialize() {
  const original = new Database(':memory:');
  original.exec("create table t(key integer primary key, value text); insert into t values (1, 'hello')");
  const bytes = original.serialize();
  assert.ok(bytes instanceof Uint8Array);
  assert.ok(bytes.length > 0);
  assert.strictEqual(String.fromCharCode(...bytes.subarray(0, 15)), 'SQLite format 3');
  original.close();

  const clone = new Database(':memory:');
  const stale = clone.prepare('select 1');
  clone.deserialize(bytes);
  assert.throws(() => stale.get(), { code: 'ERR_INVALID_STATE' });
  assert.deepStrictEqual(plain(clone.prepare('select value from t').get()), { value: 'hello' });
  clone.exec("insert into t values (2, 'world')");
  assert.strictEqual(clone.prepare('select count(*) as n from t').get().n, 2);
  assert.throws(() => clone.deserialize('nope'), { code: 'ERR_INVALID_ARG_TYPE' });
  clone.close();
}

function testSessions() {
  const source = new Database(':memory:');
  const target = new Database(':memory:');
  const schema = 'create table data(key integer primary key, value text); create table other(key integer primary key, value text)';
  source.exec(schema);
  target.exec(schema);

  const session = source.createSession();
  const scoped = source.createSession({ table: 'other' });
  const insert = source.prepare('insert into data(key, value) values (?, ?)');
  insert.run(1, 'hello');
  insert.run(2, 'world');
  source.exec("insert into other values (1, 'x')");

  const changeset = session.changeset();
  const patchset = session.patchset();
  assert.ok(changeset instanceof Uint8Array && changeset.length > 0);
  assert.ok(patchset instanceof Uint8Array && patchset.length > 0);
  assert.ok(scoped.changeset().length < changeset.length);

  const filtered = [];
  assert.strictEqual(target.applyChangeset(changeset, { filter: table => (filtered.push(table), table === 'data') }), true);
  assert.deepStrictEqual(filtered.sort(), ['data', 'other']);
  assert.deepStrictEqual(target.prepare('select * from data order by key').all().map(plain), [
    { key: 1, value: 'hello' }, { key: 2, value: 'world' }
  ]);
  assert.strictEqual(target.prepare('select count(*) as n from other').get().n, 0);

  const conflicts = [];
  assert.strictEqual(target.applyChangeset(changeset, {
    filter: table => table === 'data',
    onConflict: type => (conflicts.push(type), constants.SQLITE_CHANGESET_OMIT)
  }), true);
  assert.deepStrictEqual(conflicts, [constants.SQLITE_CHANGESET_CONFLICT, constants.SQLITE_CHANGESET_CONFLICT]);
  assert.strictEqual(target.applyChangeset(changeset, { filter: table => table === 'data' }), false);

  const boom = new Error('conflict failed');
  assert.throws(() => target.applyChangeset(changeset, { filter: table => table === 'data', onConflict: () => { throw boom; } }), error => error === boom);
  assert.throws(() => target.applyChangeset('nope'), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => target.applyChangeset(changeset, { filter: 1 }), { code: 'ERR_INVALID_ARG_TYPE' });

  session.close();
  assert.throws(() => session.changeset(), { code: 'ERR_INVALID_STATE' });
  assert.throws(() => session.close(), { code: 'ERR_INVALID_STATE' });
  scoped[Symbol.dispose]();
  scoped[Symbol.dispose]();

  const orphan = source.createSession();
  source.close();
  assert.throws(() => orphan.changeset(), { code: 'ERR_INVALID_STATE' });
  target.close();
}

function testTagStore() {
  const db = new Database(':memory:');
  const sql = db.createTagStore(2);
  assert.strictEqual(sql.constructor.name, 'SQLTagStore');
  assert.strictEqual(sql.db, db);
  assert.strictEqual(sql.capacity, 2);
  assert.strictEqual(sql.size, 0);

  db.exec('create table users(id int, name text)');
  assert.deepStrictEqual(sql.run`insert into users values (1, 'Alice')`, { changes: 1, lastInsertRowid: 1 });
  sql.run`insert into users values (${2}, ${'Bob'})`;
  assert.strictEqual(sql.size, 2);

  const name = 'Alice';
  assert.deepStrictEqual(plain(sql.get`select * from users where name = ${name}`), { id: 1, name: 'Alice' });
  assert.strictEqual(sql.size, 2);
  assert.deepStrictEqual(plain(sql.get`select * from users where name = ${'Bob'}`), { id: 2, name: 'Bob' });
  assert.strictEqual(sql.size, 2);
  assert.deepStrictEqual(sql.all`select * from users order by id`.map(plain), [{ id: 1, name: 'Alice' }, { id: 2, name: 'Bob' }]);
  assert.deepStrictEqual([...sql.iterate`select id from users order by id`].map(r => r.id), [1, 2]);

  sql.clear();
  assert.strictEqual(sql.size, 0);
  assert.strictEqual(db.createTagStore().capacity, 1000);
  db.close();
}

function testModules() {
  const db = new Database(':memory:');

  db.createModule('generate_series', {
    columns: [
      { name: 'value', type: 'INTEGER' },
      { name: 'start', type: 'INTEGER', hidden: true },
      { name: 'stop', type: 'INTEGER', hidden: true },
      { name: 'step', type: 'INTEGER', hidden: true }
    ],
    *rows(start, stop, step) {
      start ??= 0;
      stop ??= 10;
      step ??= 1;
      for (let i = start; i <= stop; i += step) yield [i];
    }
  });

  assert.deepStrictEqual(db.prepare('select * from generate_series(1, 5, 1)').all().map(plain), [
    { value: 1 }, { value: 2 }, { value: 3 }, { value: 4 }, { value: 5 }
  ]);
  assert.deepStrictEqual(db.prepare('select value from generate_series(2, 8, 3)').all().map(r => r.value), [2, 5, 8]);
  assert.strictEqual(db.prepare('select count(*) as n from generate_series').get().n, 11);
  assert.strictEqual(db.prepare('select sum(value) as n from generate_series(1, 4)').get().n, 10);

  db.exec('create virtual table series using generate_series');
  assert.strictEqual(db.prepare('select count(*) as n from series where start = 3 and stop = 5').get().n, 3);

  db.createModule('people', {
    columns: [{ name: 'name', type: 'TEXT' }, { name: 'age', type: 'INTEGER' }, { name: 'tag', type: 'ANY' }],
    rows: () => [['ann', 30n, null], ['bob', 25.5, new Uint8Array([1])]]
  });
  assert.deepStrictEqual(db.prepare('select name, age, typeof(age) as kind, tag from people order by name').all().map(plain), [
    { name: 'ann', age: 30, kind: 'integer', tag: null },
    { name: 'bob', age: 25.5, kind: 'real', tag: new Uint8Array([1]) }
  ]);
  assert.deepStrictEqual(
    db.prepare('select p.name, s.value from people p join generate_series(1, 2) s order by 1, 2').all().map(r => `${r.name}${r.value}`),
    ['ann1', 'ann2', 'bob1', 'bob2']
  );

  db.createModule('typed', {
    useBigIntArguments: true,
    columns: [{ name: 'kind', type: 'TEXT' }, { name: 'input', type: 'INTEGER', hidden: true }],
    rows: input => [[typeof input]]
  });
  assert.strictEqual(db.prepare('select kind from typed(5)').get().kind, 'bigint');

  const boom = new Error('rows failed');
  db.createModule('broken', { columns: [{ name: 'a', type: 'INTEGER' }], rows: () => { throw boom; } });
  assert.throws(() => db.prepare('select * from broken').all(), error => error === boom);

  assert.throws(() => db.createModule('x', { columns: [], rows: () => [] }), { code: 'ERR_INVALID_ARG_VALUE' });
  assert.throws(() => db.createModule('x', { columns: [{ name: 'a', type: 'NOPE' }], rows: () => [] }), { code: 'ERR_INVALID_ARG_VALUE' });
  assert.throws(() => db.createModule('x', { columns: [{ name: 'a', type: 'TEXT' }] }), { code: 'ERR_INVALID_ARG_TYPE' });
  db.close();
}

function testDefensiveAndExtensions() {
  const db = new Database(':memory:');
  db.enableDefensive(false);
  db.enableDefensive(true);
  assert.throws(() => db.enableDefensive(1), { code: 'ERR_INVALID_ARG_TYPE' });
  assert.throws(() => db.enableLoadExtension(true), { code: 'ERR_INVALID_STATE' });
  assert.throws(() => db.loadExtension('nope'), { code: 'ERR_INVALID_STATE' });
  db.enableLoadExtension(false);
  db.close();

  const strict = new Database(':memory:');
  assert.throws(() => strict.exec('select "nope"'), { code: 'ERR_SQLITE_ERROR' });
  strict.close();

  const loose = new Database(':memory:', { enableDoubleQuotedStringLiterals: true });
  assert.strictEqual(loose.prepare('select "text" as v').get().v, 'text');
  loose.close();

  const keyed = new Database(':memory:');
  keyed.exec('create table p(id integer primary key); create table c(p references p(id))');
  assert.throws(() => keyed.exec('insert into c values (5)'), { code: 'ERR_SQLITE_ERROR' });
  keyed.close();

  const unkeyed = new Database(':memory:', { enableForeignKeyConstraints: false });
  unkeyed.exec('create table p(id integer primary key); create table c(p references p(id)); insert into c values (5)');
  unkeyed.close();
}

async function testBackup() {
  const source = new Database(':memory:');
  source.exec('create table t(a, b)');
  const insert = source.prepare('insert into t values (?, ?)');
  for (let i = 0; i < 400; i++) insert.run(i, 'x'.repeat(100));

  const file = path.join(dir, 'backup.db');
  const progress = [];
  const total = await backup(source, file, { rate: 1, progress: state => progress.push(state) });

  assert.ok(total > 1);
  assert.ok(progress.length >= 1);
  assert.strictEqual(progress[0].totalPages, total);
  assert.ok(progress[0].remainingPages < total);

  const copy = new Database(file);
  assert.strictEqual(copy.prepare('select count(*) as n from t').get().n, 400);
  copy.close();

  assert.strictEqual(await backup(source, pathToFileURL(path.join(dir, 'backup2.db'))), total);
  assert.throws(() => backup(source, file, { rate: 'x' }), { code: 'ERR_INVALID_ARG_TYPE' });
  await assert.rejects(backup(source, path.join(dir, 'missing', 'x.db')), { code: 'ERR_SQLITE_ERROR' });
  source.close();
}

function testDiagnostics() {
  const dc = require('node:diagnostics_channel');
  const db = new Database(':memory:');
  const events = [];
  const onQuery = message => events.push(message);

  db.exec('create table t(a)');
  dc.subscribe('sqlite.db.query', onQuery);
  db.prepare('insert into t values (?)').run(5);
  db.prepare('select a from t where a = ?').all(5);
  dc.unsubscribe('sqlite.db.query', onQuery);
  db.prepare('select 1').get();

  assert.strictEqual(events.length, 2);
  assert.strictEqual(events[0].sql, 'insert into t values (5.0)');
  assert.strictEqual(events[1].sql, 'select a from t where a = 5.0');
  assert.strictEqual(events[0].database, db);
  assert.strictEqual(typeof events[0].duration, 'number');
  db.close();
}

async function main() {
  testExports();
  testOpenClose();
  testFiles();
  testBindingAndReading();
  testColumnsStatsAndClose();
  testIterate();
  testFunctions();
  testAggregates();
  testAuthorizer();
  testLimits();
  testSerialize();
  testSessions();
  testTagStore();
  testModules();
  testDefensiveAndExtensions();
  await testBackup();
  testDiagnostics();

  fs.rmSync(dir, { recursive: true, force: true });
  console.log('ok');
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
