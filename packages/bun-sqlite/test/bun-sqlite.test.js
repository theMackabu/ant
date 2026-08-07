// Runs on Ant (ant test/bun-sqlite.test.js) and Node 22.5+.
import Database, { Database as NamedDatabase, constants } from '../src/index.js';

function assert(condition, message) {
  if (!condition) { console.log('FAIL:', message); process.exit(1); }
}

assert(Database === NamedDatabase, 'default and named export are the same class');
assert(typeof constants.SQLITE_OPEN_READONLY === 'number', 'constants exported');

// default in-memory database, query cache, run/get/all/values
{
  const db = new Database();
  assert(db.filename === ':memory:', `default filename, got ${db.filename}`);
  db.run('CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL)');
  db.run('INSERT INTO t (name, score) VALUES (?, ?)', 'alpha', 1.5);
  db.run('INSERT INTO t (name, score) VALUES ($name, $score)', { $name: 'beta', $score: 2.5 });

  assert(db.query('SELECT * FROM t') === db.query('SELECT * FROM t'), 'query() caches statements');

  const row = db.query('SELECT * FROM t WHERE name = ?').get('alpha');
  assert(row !== null && row.name === 'alpha' && row.score === 1.5, 'get() returns row');
  assert(db.query('SELECT * FROM t WHERE name = ?').get('missing') === null,
    'get() miss returns null (bun semantics, not undefined)');

  const rows = db.query('SELECT name FROM t ORDER BY id').all();
  assert(rows.length === 2 && rows[0].name === 'alpha', 'all()');

  const vals = db.query('SELECT id, name FROM t ORDER BY id').values();
  assert(Array.isArray(vals[0]) && vals[0][1] === 'alpha' && vals[1][1] === 'beta',
    `values() returns arrays, got ${JSON.stringify(vals)}`);
  db.close();
}

// undefined binds as NULL (bun semantics; node:sqlite would throw)
{
  const db = new Database(':memory:');
  db.run('CREATE TABLE u (v)');
  db.run('INSERT INTO u VALUES (?)', undefined);
  assert(db.query('SELECT v FROM u').get().v === null, 'undefined binds as NULL');
  db.close();
}

// iteration + as(Class)
{
  class Score {
    doubled() { return this.v * 2; }
  }
  const db = new Database(':memory:');
  db.run('CREATE TABLE s (v INTEGER)');
  const tx = db.transaction(rows => { for (const v of rows) db.run('INSERT INTO s VALUES (?)', v); });
  tx([1, 2, 3]);

  const collected = [];
  for (const row of db.query('SELECT v FROM s ORDER BY v')) collected.push(row.v);
  assert(collected.join(',') === '1,2,3', `statement is iterable, got ${collected}`);

  const mapped = db.query('SELECT v FROM s ORDER BY v').as(Score).get();
  assert(mapped instanceof Score && mapped.doubled() === 2, 'as(Class) maps row prototypes');
  db.close();
}

// transactions: commit, rollback, nesting via savepoints, variants
{
  const db = new Database(':memory:');
  db.run('CREATE TABLE tx (v INTEGER)');

  const insert = db.transaction(v => { db.run('INSERT INTO tx VALUES (?)', v); return v; });
  assert(insert(1) === 1, 'transaction returns fn result');
  assert(db.inTransaction === false, 'transaction commits');

  const failing = db.transaction(() => {
    db.run('INSERT INTO tx VALUES (2)');
    throw new Error('boom');
  });
  let threw = false;
  try { failing(); } catch { threw = true; }
  assert(threw, 'failing transaction rethrows');
  assert(db.query('SELECT COUNT(*) AS c FROM tx').get().c === 1, 'failed transaction rolls back');

  const outer = db.transaction(() => {
    db.run('INSERT INTO tx VALUES (3)');
    const inner = db.transaction(() => { db.run('INSERT INTO tx VALUES (4)'); throw new Error('inner'); });
    try { inner(); } catch {}
    db.run('INSERT INTO tx VALUES (5)');
  });
  outer();
  const vs = db.query('SELECT v FROM tx ORDER BY v').values().map(r => r[0]);
  assert(vs.join(',') === '1,3,5', `nested savepoint rollback, got ${vs}`);

  db.transaction(() => db.run('INSERT INTO tx VALUES (6)')).immediate();
  assert(db.query('SELECT COUNT(*) AS c FROM tx').get().c === 4, 'immediate variant works');
  db.close();
}

// safeIntegers: constructor option and per-statement toggle
{
  const db = new Database(':memory:', { safeIntegers: true });
  db.run('CREATE TABLE big (v INTEGER)');
  db.run('INSERT INTO big VALUES (?)', 9007199254740993n);
  assert(db.query('SELECT v FROM big').get().v === 9007199254740993n, 'safeIntegers db option');
  db.close();

  const db2 = new Database(':memory:');
  db2.run('CREATE TABLE big (v INTEGER)');
  db2.run('INSERT INTO big VALUES (?)', 12n);
  assert(db2.query('SELECT v FROM big').safeIntegers(true).get().v === 12n, 'safeIntegers(true) toggle');
  db2.close();
}

// create: false on a missing file throws
{
  let threw = false;
  try { new Database('/tmp/ant_bun_sqlite_missing_' + process.pid + '.db', { create: false }); }
  catch (e) { threw = e.code === 'SQLITE_CANTOPEN'; }
  assert(threw, 'create:false on missing file throws SQLITE_CANTOPEN');
}

// foreign keys stay off by default (bun/SQLite default, unlike node:sqlite)
{
  const db = new Database(':memory:');
  db.run('CREATE TABLE parent (id INTEGER PRIMARY KEY)');
  db.run('CREATE TABLE child (pid INTEGER REFERENCES parent(id))');
  db.run('INSERT INTO child VALUES (99)'); // would throw with FK enforcement on
  assert(db.query('SELECT COUNT(*) AS c FROM child').get().c === 1, 'FK off by default like bun');
  db.close();
}

// columnNames, toString, finalize, close semantics
{
  const db = new Database(':memory:');
  db.run('CREATE TABLE c (a INTEGER, b TEXT)');
  const stmt = db.prepare('SELECT a, b FROM c WHERE a = ?');
  assert(stmt.columnNames.join(',') === 'a,b', 'columnNames');
  stmt.get(7);
  assert(stmt.toString().includes('7'), 'toString() is expanded SQL');

  stmt.finalize();
  let threw = false;
  try { stmt.get(1); } catch { threw = true; }
  assert(threw, 'finalized statement throws on use');

  db.close();
  db.close(); // idempotent without throwOnError
  threw = false;
  try { db.close(true); } catch { threw = true; }
  assert(threw, 'close(true) on closed db throws');
  threw = false;
  try { db.query('SELECT 1'); } catch { threw = true; }
  assert(threw, 'query on closed db throws');
}

// unsupported APIs are loud
{
  const db = new Database(':memory:');
  for (const call of [() => db.serialize(), () => db.loadExtension('x'), () => db.fileControl(0),
                      () => Database.deserialize(new Uint8Array())]) {
    let threw = false;
    try { call(); } catch (e) { threw = e.code === 'ERR_UNSUPPORTED'; }
    assert(threw, 'unsupported API should throw ERR_UNSUPPORTED');
  }
  db.close();
}

console.log('PASS');
