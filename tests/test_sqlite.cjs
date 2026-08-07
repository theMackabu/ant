// ant:sqlite — node:sqlite-shaped DatabaseSync/StatementSync surface.
// Covers typed round-trips, anonymous and named binding, run/get/all/iterate,
// bigint handling, transactions, and handle lifecycle errors.
function assert(condition, message) {
  if (!condition) { console.log('FAIL:', message); process.exit(1); }
}

const { DatabaseSync, StatementSync } = require('ant:sqlite');
const nodeSqlite = require('node:sqlite');
assert(nodeSqlite.DatabaseSync === DatabaseSync, 'node:sqlite aliases ant:sqlite');

// in-memory open, location(), isOpen
{
  const db = new DatabaseSync(':memory:');
  assert(db.isOpen === true, 'db should be open after construction');
  assert(db.location() === null, 'in-memory db location() should be null');
  db.close();
  assert(db.isOpen === false, 'db should report closed');
}

// deferred open via { open: false }
{
  const db = new DatabaseSync(':memory:', { open: false });
  assert(db.isOpen === false, 'db should not open with open:false');
  db.open();
  assert(db.isOpen === true, 'db.open() should open');
  db.close();
}

// exec + prepare + run/get/all with typed values
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL, data BLOB, flag INTEGER)');

  const insert = db.prepare('INSERT INTO t (name, score, data, flag) VALUES (?, ?, ?, ?)');
  const r1 = insert.run('alpha', 1.5, new Uint8Array([1, 2, 3]), true);
  assert(r1.changes === 1, `insert changes should be 1, got ${r1.changes}`);
  assert(r1.lastInsertRowid === 1, `first rowid should be 1, got ${r1.lastInsertRowid}`);

  insert.run('beta', -2.25, new Uint8Array([4, 5]), false);

  const row = db.prepare('SELECT * FROM t WHERE name = ?').get('alpha');
  assert(row !== undefined, 'get should find alpha');
  assert(row.id === 1, `id should be 1, got ${row.id}`);
  assert(row.name === 'alpha', `name should round-trip, got ${row.name}`);
  assert(row.score === 1.5, `score should round-trip, got ${row.score}`);
  assert(row.flag === 1, `boolean should bind as 1, got ${row.flag}`);
  assert(row.data instanceof Uint8Array, 'blob should read back as Uint8Array');
  assert(row.data.length === 3 && row.data[0] === 1 && row.data[2] === 3, 'blob bytes should round-trip');

  const all = db.prepare('SELECT name FROM t ORDER BY id').all();
  assert(all.length === 2, `all() should return 2 rows, got ${all.length}`);
  assert(all[0].name === 'alpha' && all[1].name === 'beta', 'all() rows should be ordered');

  const missing = db.prepare('SELECT * FROM t WHERE name = ?').get('nope');
  assert(missing === undefined, 'get() with no match should return undefined');

  db.close();
}

// NULL round-trip
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE n (v)');
  db.prepare('INSERT INTO n VALUES (?)').run(null);
  const row = db.prepare('SELECT v FROM n').get();
  assert(row.v === null, `NULL should read back as null, got ${JSON.stringify(row.v)}`);
  db.close();
}

// named parameters: prefixed and bare keys
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE p (a, b)');
  const stmt = db.prepare('INSERT INTO p VALUES (:a, @b)');
  stmt.run({ ':a': 1, '@b': 2 });
  stmt.run({ a: 3, b: 4 }); // bare names allowed by default
  const rows = db.prepare('SELECT a, b FROM p ORDER BY a').all();
  assert(rows.length === 2, `named binds should insert 2 rows, got ${rows.length}`);
  assert(rows[0].a === 1 && rows[0].b === 2, 'prefixed named params should bind');
  assert(rows[1].a === 3 && rows[1].b === 4, 'bare named params should bind');

  // unknown named parameter throws by default
  let threw = false;
  try { stmt.run({ nosuch: 1 }); } catch (e) { threw = true; }
  assert(threw, 'unknown named parameter should throw');

  // ...unless explicitly allowed
  stmt.setAllowUnknownNamedParameters(true);
  stmt.run({ a: 5, b: 6, nosuch: 7 });
  db.close();
}

// mixed named + anonymous
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE m (a, b, c)');
  db.prepare('INSERT INTO m VALUES (:a, ?, ?)').run({ a: 'x' }, 'y', 'z');
  const row = db.prepare('SELECT * FROM m').get();
  assert(row.a === 'x' && row.b === 'y' && row.c === 'z', 'mixed named+anonymous binding');
  db.close();
}

// bigint: bind + read back, setReadBigInts
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE big (v INTEGER)');
  db.prepare('INSERT INTO big VALUES (?)').run(9007199254740993n); // 2^53 + 1

  const plain = db.prepare('SELECT v FROM big');
  let threw = false;
  try { plain.get(); } catch (e) { threw = true; }
  assert(threw, 'reading unsafe integer without readBigInts should throw');

  const bigStmt = db.prepare('SELECT v FROM big');
  bigStmt.setReadBigInts(true);
  const row = bigStmt.get();
  assert(row.v === 9007199254740993n, `bigint should round-trip, got ${row.v}`);

  // out-of-int64-range bigint binding must throw
  threw = false;
  try { db.prepare('INSERT INTO big VALUES (?)').run(2n ** 64n); } catch (e) { threw = true; }
  assert(threw, 'binding a >64-bit bigint should throw');
  db.close();
}

// iterate()
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE seq (v INTEGER)');
  const ins = db.prepare('INSERT INTO seq VALUES (?)');
  for (let i = 1; i <= 5; i++) ins.run(i);

  const collected = [];
  for (const row of db.prepare('SELECT v FROM seq ORDER BY v').iterate()) {
    collected.push(row.v);
  }
  assert(collected.join(',') === '1,2,3,4,5', `iterate should yield rows in order, got ${collected}`);

  // early break releases the statement for reuse
  const stmt = db.prepare('SELECT v FROM seq ORDER BY v');
  for (const row of stmt.iterate()) {
    if (row.v === 2) break;
  }
  const again = stmt.all();
  assert(again.length === 5, `statement should be reusable after early break, got ${again.length}`);
  db.close();
}

// sourceSQL / expandedSQL / columns()
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE s (id INTEGER, label TEXT)');
  const stmt = db.prepare('SELECT id, label FROM s WHERE id = ?');
  assert(stmt.sourceSQL === 'SELECT id, label FROM s WHERE id = ?', `sourceSQL, got ${stmt.sourceSQL}`);
  stmt.get(42);
  assert(stmt.expandedSQL.includes('42'), `expandedSQL should include bound value, got ${stmt.expandedSQL}`);

  const cols = stmt.columns();
  assert(cols.length === 2, `columns() should describe 2 columns, got ${cols.length}`);
  assert(cols[0].name === 'id' && cols[1].name === 'label', 'columns() names');
  assert(cols[0].table === 's', `columns() table, got ${cols[0].table}`);
  assert(cols[0].type === 'INTEGER', `columns() decltype, got ${cols[0].type}`);
  db.close();
}

// transactions + isTransaction
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE tx (v)');
  assert(db.isTransaction === false, 'no transaction initially');
  db.exec('BEGIN');
  assert(db.isTransaction === true, 'isTransaction inside BEGIN');
  db.prepare('INSERT INTO tx VALUES (1)').run();
  db.exec('ROLLBACK');
  assert(db.isTransaction === false, 'no transaction after ROLLBACK');
  assert(db.prepare('SELECT COUNT(*) AS c FROM tx').get().c === 0, 'rollback should discard insert');
  db.close();
}

// foreign keys are enforced by default
{
  const db = new DatabaseSync(':memory:');
  db.exec('CREATE TABLE parent (id INTEGER PRIMARY KEY)');
  db.exec('CREATE TABLE child (pid INTEGER REFERENCES parent(id))');
  let threw = false;
  try { db.exec('INSERT INTO child VALUES (99)'); } catch (e) { threw = true; }
  assert(threw, 'foreign key violation should throw by default');
  db.close();
}

// error shape: code / errcode / errstr
{
  const db = new DatabaseSync(':memory:');
  let err = null;
  try { db.exec('NOT VALID SQL'); } catch (e) { err = e; }
  assert(err !== null, 'invalid SQL should throw');
  assert(err.code === 'ERR_SQLITE_ERROR', `error code, got ${err.code}`);
  assert(typeof err.errcode === 'number', `errcode should be numeric, got ${typeof err.errcode}`);
  assert(typeof err.errstr === 'string', `errstr should be a string, got ${typeof err.errstr}`);
  db.close();
}

// lifecycle errors
{
  const db = new DatabaseSync(':memory:');
  const stmt = db.prepare('SELECT 1 AS one');
  db.close();

  let threw = false;
  try { stmt.get(); } catch (e) { threw = true; }
  assert(threw, 'statement use after db.close() should throw');

  threw = false;
  try { db.close(); } catch (e) { threw = true; }
  assert(threw, 'double close should throw');

  threw = false;
  try { db.exec('SELECT 1'); } catch (e) { threw = true; }
  assert(threw, 'exec on closed db should throw');

  threw = false;
  try { new StatementSync(); } catch (e) { threw = true; }
  assert(threw, 'StatementSync should not be directly constructible');

  threw = false;
  try { DatabaseSync(':memory:'); } catch (e) { threw = true; }
  assert(threw, 'DatabaseSync without new should throw');
}

// file-backed database round-trip
{
  const fs = require('node:fs');
  const path = '/tmp/ant_test_sqlite_' + process.pid + '.db';
  try {
    const db = new DatabaseSync(path);
    assert(db.location() === path, `file db location(), got ${db.location()}`);
    db.exec('CREATE TABLE f (v TEXT)');
    db.prepare('INSERT INTO f VALUES (?)').run('persisted');
    db.close();

    const again = new DatabaseSync(path, { readOnly: true });
    assert(again.prepare('SELECT v FROM f').get().v === 'persisted', 'file db should persist');
    let threw = false;
    try { again.exec('INSERT INTO f VALUES (\'nope\')'); } catch (e) { threw = true; }
    assert(threw, 'write on readOnly db should throw');
    again.close();
  } finally {
    try { fs.unlinkSync(path); } catch {}
  }
}

// process.versions.sqlite
assert(typeof process.versions.sqlite === 'string' && process.versions.sqlite.startsWith('3.'),
  `process.versions.sqlite should report 3.x, got ${process.versions.sqlite}`);

console.log('PASS');
