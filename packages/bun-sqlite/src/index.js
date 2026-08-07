// bun:sqlite-compatible API over node:sqlite (which Ant serves natively as
// ant:sqlite). Runs on Ant and on Node 22.5+.
//
// Differences from bun:sqlite are loud, not silent: unsupported APIs throw
// with an explanation instead of approximating behavior.

import { DatabaseSync } from 'node:sqlite';
import { existsSync } from 'node:fs';

export const constants = {
  SQLITE_OPEN_READONLY: 0x00000001,
  SQLITE_OPEN_READWRITE: 0x00000002,
  SQLITE_OPEN_CREATE: 0x00000004,
  SQLITE_OPEN_URI: 0x00000040,
  SQLITE_OPEN_MEMORY: 0x00000080,
};

const kDispose = Symbol.dispose ?? Symbol.for('nodejs.dispose');

function unsupported(name, why) {
  const err = new Error(`${name} is not supported by @ant/bun-sqlite: ${why}`);
  err.code = 'ERR_UNSUPPORTED';
  return err;
}

// bun binds `undefined` as NULL; node:sqlite rejects it. Normalize positional
// args and named-bag values on the way through.
function normalizeArgs(args) {
  return args.map(arg => {
    if (arg === undefined) return null;
    if (arg !== null && typeof arg === 'object' && !ArrayBuffer.isView(arg) &&
        !(arg instanceof ArrayBuffer) && Object.getPrototypeOf(arg) === Object.prototype) {
      const bag = {};
      for (const key of Object.keys(arg)) {
        bag[key] = arg[key] === undefined ? null : arg[key];
      }
      return bag;
    }
    return arg;
  });
}

export class Statement {
  #stmt;
  #db;
  #finalized = false;
  #safeIntegers;
  #rowClass = null;
  #defaultArgs;

  constructor(stmt, db, safeIntegers, defaultArgs) {
    this.#stmt = stmt;
    this.#db = db;
    this.#safeIntegers = !!safeIntegers;
    this.#defaultArgs = defaultArgs?.length ? defaultArgs : null;
    if (this.#safeIntegers) stmt.setReadBigInts(true);
  }

  #live() {
    if (this.#finalized) throw new Error('Statement has been finalized');
    return this.#stmt;
  }

  #args(args) {
    if (args.length === 0 && this.#defaultArgs) return this.#defaultArgs;
    return normalizeArgs(args);
  }

  #shape(row) {
    if (row === undefined) return null;
    if (this.#rowClass) Object.setPrototypeOf(row, this.#rowClass.prototype);
    return row;
  }

  get(...args) {
    return this.#shape(this.#live().get(...this.#args(args)));
  }

  all(...args) {
    const rows = this.#live().all(...this.#args(args));
    if (this.#rowClass) {
      for (const row of rows) Object.setPrototypeOf(row, this.#rowClass.prototype);
    }
    return rows;
  }

  // Rows as arrays, in column order. Duplicate result-column names collapse
  // (rows come back as objects from node:sqlite); alias columns to avoid that.
  values(...args) {
    const stmt = this.#live();
    const names = stmt.columns().map(c => c.name);
    return stmt.all(...this.#args(args)).map(row => names.map(n => row[n]));
  }

  run(...args) {
    return this.#live().run(...this.#args(args));
  }

  *iterate(...args) {
    for (const row of this.#live().iterate(...this.#args(args))) {
      yield this.#shape(row);
    }
  }

  [Symbol.iterator]() {
    return this.iterate();
  }

  as(Class) {
    this.#rowClass = Class;
    return this;
  }

  safeIntegers(enabled = true) {
    this.#safeIntegers = !!enabled;
    this.#live().setReadBigInts(this.#safeIntegers);
    return this;
  }

  finalize() {
    // node:sqlite frees the underlying statement on GC; this makes reuse loud.
    this.#finalized = true;
    this.#db?._uncache(this);
    this.#stmt = null;
  }

  toString() {
    return this.#live().expandedSQL;
  }

  get columnNames() {
    return this.#live().columns().map(c => c.name);
  }

  get paramsCount() {
    throw unsupported('Statement.paramsCount', 'node:sqlite does not expose bind parameter counts');
  }

  get native() {
    throw unsupported('Statement.native', 'there is no native handle to expose');
  }

  [kDispose]() {
    this.finalize();
  }
}

export class Database {
  #db;
  #open = false;
  #cache = new Map();
  #cacheKeys = new Map();
  #safeIntegers = false;
  #strict = false;
  #txDepth = 0;
  filename;

  constructor(filename = ':memory:', options = {}) {
    if (filename === '' || filename === undefined || filename === null) filename = ':memory:';
    this.filename = filename;

    let readonly = false;
    let create = true;
    let safeIntegers = false;
    let strict = false;

    if (typeof options === 'number') {
      readonly = (options & constants.SQLITE_OPEN_READONLY) !== 0;
      create = (options & constants.SQLITE_OPEN_CREATE) !== 0 || readonly === false && options === 0;
      if ((options & constants.SQLITE_OPEN_MEMORY) !== 0) filename = ':memory:';
    } else if (options && typeof options === 'object') {
      readonly = !!options.readonly;
      if ('create' in options) create = !!options.create;
      safeIntegers = !!options.safeIntegers;
      strict = !!options.strict;
    }

    const isMemory = filename === ':memory:' || filename.startsWith('file::memory:');
    if (!create && !readonly && !isMemory && !existsSync(filename)) {
      const err = new Error(`unable to open database file: ${filename}`);
      err.code = 'SQLITE_CANTOPEN';
      throw err;
    }

    // Match bun: SQLite's default of foreign_keys=OFF is preserved
    // (node:sqlite's DatabaseSync turns it on by default).
    this.#db = new DatabaseSync(filename, {
      readOnly: readonly,
      enableForeignKeyConstraints: false,
    });
    this.#open = true;
    this.#safeIntegers = safeIntegers;
    this.#strict = strict;
  }

  get strict() {
    return this.#strict;
  }

  #live() {
    if (!this.#open) throw new Error('Database has been closed');
    return this.#db;
  }

  prepare(sql, ...defaultArgs) {
    const stmt = this.#live().prepare(sql);
    return new Statement(stmt, this, this.#safeIntegers, normalizeArgs(defaultArgs));
  }

  query(sql) {
    let cached = this.#cache.get(sql);
    if (!cached) {
      cached = this.prepare(sql);
      this.#cache.set(sql, cached);
      this.#cacheKeys.set(cached, sql);
    }
    return cached;
  }

  _uncache(stmt) {
    const key = this.#cacheKeys.get(stmt);
    if (key !== undefined) {
      this.#cache.delete(key);
      this.#cacheKeys.delete(stmt);
    }
  }

  run(sql, ...args) {
    return this.prepare(sql).run(...args);
  }

  exec(sql, ...args) {
    if (args.length === 0) {
      // Multi-statement scripts only work through DatabaseSync.exec.
      this.#live().exec(sql);
      return { changes: 0, lastInsertRowid: 0 };
    }
    return this.run(sql, ...args);
  }

  transaction(fn) {
    const db = this;
    const wrap = mode => (...args) => {
      const live = db.#live();
      const nested = db.#txDepth > 0;
      const name = `_ant_tx_${db.#txDepth}`;
      live.exec(nested ? `SAVEPOINT ${name}` : `BEGIN ${mode}`);
      db.#txDepth++;
      try {
        const result = fn(...args);
        db.#txDepth--;
        live.exec(nested ? `RELEASE ${name}` : 'COMMIT');
        return result;
      } catch (err) {
        db.#txDepth--;
        live.exec(nested ? `ROLLBACK TO ${name}; RELEASE ${name}` : 'ROLLBACK');
        throw err;
      }
    };

    const tx = wrap('DEFERRED');
    tx.deferred = wrap('DEFERRED');
    tx.immediate = wrap('IMMEDIATE');
    tx.exclusive = wrap('EXCLUSIVE');
    return tx;
  }

  get inTransaction() {
    return this.#open && this.#db.isTransaction;
  }

  close(throwOnError = false) {
    if (!this.#open) {
      if (throwOnError) throw new Error('Database has been closed');
      return;
    }
    this.#open = false;
    this.#cache.clear();
    this.#cacheKeys.clear();
    try {
      this.#db.close();
    } catch (err) {
      if (throwOnError) throw err;
    }
  }

  serialize() {
    throw unsupported('Database.serialize()', 'the sqlite3_serialize API is not exposed by ant:sqlite');
  }

  static deserialize() {
    throw unsupported('Database.deserialize()', 'the sqlite3_deserialize API is not exposed by ant:sqlite');
  }

  loadExtension() {
    throw unsupported('Database.loadExtension()', "Ant's SQLite is built with SQLITE_OMIT_LOAD_EXTENSION");
  }

  fileControl() {
    throw unsupported('Database.fileControl()', 'the sqlite3_file_control API is not exposed by ant:sqlite');
  }

  [kDispose]() {
    this.close();
  }
}

export default Database;
