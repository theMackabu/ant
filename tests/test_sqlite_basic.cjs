const assert = require('node:assert');
const { DatabaseSync } = require('node:sqlite');

const db = new DatabaseSync(':memory:');
assert.equal(db.isOpen, true);

db.exec('CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL, data BLOB)');

const insert = db.prepare('INSERT INTO t(name, score, data) VALUES (?, ?, ?)');
assert.deepStrictEqual({ ...insert.run('a', 1.5, new Uint8Array([1, 2, 3])) }, { changes: 1, lastInsertRowid: 1 });
assert.deepStrictEqual({ ...insert.run('b', 2, null) }, { changes: 1, lastInsertRowid: 2 });
assert.equal(insert.sourceSQL, 'INSERT INTO t(name, score, data) VALUES (?, ?, ?)');

const first = db.prepare('SELECT * FROM t WHERE id = ?').get(1);
assert.equal(first.id, 1);
assert.equal(first.name, 'a');
assert.equal(first.score, 1.5);
assert.deepStrictEqual([...first.data], [1, 2, 3]);

const named = db.prepare('SELECT name FROM t WHERE score >= :min AND name != $skip ORDER BY id');
assert.deepStrictEqual(named.all({ min: 1, skip: 'zzz' }).map(row => row.name), ['a', 'b']);
assert.deepStrictEqual(named.all({ min: 2, skip: 'zzz' }).map(row => row.name), ['b']);

assert.equal(db.prepare('SELECT * FROM t WHERE id = ?').get(99), undefined);
assert.equal(db.prepare('SELECT ? AS n').get(12n).n, 12);
assert.equal(db.prepare("SELECT json_extract('{\"a\":[1,2]}', '$.a[1]') AS v").get().v, 2);

assert.throws(() => db.exec('SELECT * FROM missing'), { code: 'ERR_SQLITE_ERROR' });
assert.throws(() => db.prepare('SELECT 9223372036854775807 AS big').get(), { code: 'ERR_OUT_OF_RANGE' });
assert.throws(() => db.prepare('SELECT ?').get(Symbol('x')), { code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => DatabaseSync(':memory:'), TypeError);

const stale = db.prepare('SELECT 1 AS one');
db.close();
assert.equal(db.isOpen, false);
assert.throws(() => stale.get(), { code: 'ERR_INVALID_STATE' });
assert.throws(() => db.exec('SELECT 1'), { code: 'ERR_INVALID_STATE' });
assert.throws(() => db.close(), { code: 'ERR_INVALID_STATE' });

console.log('ok');
