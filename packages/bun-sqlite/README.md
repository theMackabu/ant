# @ant/bun-sqlite

A [bun:sqlite](https://bun.com/docs/api/sqlite)-compatible `Database` API built
on `node:sqlite`, which Ant serves natively as `ant:sqlite`. Pure JavaScript,
no build step; runs on Ant and on Node 22.5+.

```js
import { Database } from '@ant/bun-sqlite';

const db = new Database('app.db');
db.run('CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)');

const insert = db.transaction(names => {
  for (const name of names) db.run('INSERT INTO users (name) VALUES (?)', name);
});
insert(['alice', 'bob']);

db.query('SELECT * FROM users WHERE name = ?').get('alice'); // { id: 1, name: 'alice' }
```

## Supported

- `Database`: `prepare`, `query` (cached), `run`, `exec`, `transaction` with
  `.deferred`/`.immediate`/`.exclusive` and savepoint nesting, `close`,
  `inTransaction`, `filename`, `Symbol.dispose`, numeric open flags and
  `{ readonly, create, strict, safeIntegers }` options
- `Statement`: `get` (returns `null` on miss), `all`, `values`, `run`,
  `iterate`, direct iteration, `as(Class)`, `safeIntegers()`, `finalize`,
  `toString()` (expanded SQL), `columnNames`, `Symbol.dispose`
- bun binding semantics: `undefined` binds as `NULL`, booleans as `0`/`1`,
  bigints range-checked, named parameters with `$`/`:`/`@` prefixes or bare keys
- Foreign keys stay **off** by default, matching bun and SQLite themselves
  (`node:sqlite` turns them on; this shim turns them back off)

## Not supported (throws with `code: 'ERR_UNSUPPORTED'`)

- `serialize()` / `Database.deserialize()` — sqlite3_serialize is not exposed
  by the underlying module
- `loadExtension()` — Ant's SQLite is compiled with `SQLITE_OMIT_LOAD_EXTENSION`
- `fileControl()`, `Statement.native`, `Statement.paramsCount`

## Caveats

- `values()` maps object rows back to arrays by column name, so duplicate
  result-column names collapse; alias columns (`SELECT a AS a1, a AS a2`) if
  you need both.
- `exec(sql)` with no parameters runs multi-statement scripts; with parameters
  it behaves like `run(sql, ...params)` on a single statement.
