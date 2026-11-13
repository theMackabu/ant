import binding from 'ant:internal/sqlite';
import { fileURLToPath } from 'node:url';
import { channel } from 'node:diagnostics_channel';

const { Database, Statement, Session, internals } = binding;

const LIMIT_NAMES = [
  'length',
  'sqlLength',
  'column',
  'exprDepth',
  'compoundSelect',
  'vdbeOp',
  'functionArg',
  'attach',
  'likePatternLength',
  'variableNumber',
  'triggerDepth'
];

const COLUMN_TYPES = new Set(['INTEGER', 'TEXT', 'REAL', 'BLOB', 'ANY']);
const MAX_LIMIT = 0x7fffffff;
const constructToken = Symbol('sqlite.construct');

function codedError(Ctor, code, message) {
  const error = new Ctor(message);
  error.code = code;
  return error;
}

function argTypeError(name, expected) {
  return codedError(TypeError, 'ERR_INVALID_ARG_TYPE', `The "${name}" argument must be ${expected}.`);
}

function isBinary(value) {
  return ArrayBuffer.isView(value) || value instanceof ArrayBuffer || (typeof SharedArrayBuffer === 'function' && value instanceof SharedArrayBuffer);
}

function normalizePath(path, name) {
  let result;
  if (typeof path === 'string') result = path;
  else if (path instanceof Uint8Array) result = new TextDecoder().decode(path);
  else if (path instanceof URL) result = fileURLToPath(path);

  if (typeof result !== 'string' || result.includes('\0')) {
    throw codedError(TypeError, 'ERR_INVALID_ARG_TYPE', `The "${name}" argument must be a string, Uint8Array, or URL without null bytes.`);
  }
  return result;
}

function validateObject(value, name) {
  if (value === null || typeof value !== 'object') throw argTypeError(name, 'an object');
}

function validateFunction(value, name) {
  if (typeof value !== 'function') throw argTypeError(name, 'a function');
}

function validateString(value, name) {
  if (typeof value !== 'string') throw argTypeError(name, 'a string');
}

function validateBoolean(value, name) {
  if (typeof value !== 'boolean') throw argTypeError(name, 'a boolean');
}

const limitsByDatabase = new WeakMap();

function limitIndex(name) {
  return typeof name === 'string' ? LIMIT_NAMES.indexOf(name) : -1;
}

function createLimits(db) {
  return new Proxy(
    {},
    {
      get(target, name, receiver) {
        const id = limitIndex(name);
        return id === -1 ? Reflect.get(target, name, receiver) : internals.limit.call(db, id);
      },
      set(target, name, value, receiver) {
        const id = limitIndex(name);
        if (id === -1) return Reflect.set(target, name, value, receiver);

        if (typeof value !== 'number') throw argTypeError(`limits.${name}`, 'a number');
        if (value !== Infinity && (!Number.isInteger(value) || value < 0)) {
          throw codedError(RangeError, 'ERR_OUT_OF_RANGE', 'Limit value must be non-negative.');
        }

        internals.limit.call(db, id, value === Infinity || value > MAX_LIMIT ? MAX_LIMIT : value);
        return true;
      },
      has(target, name) {
        return limitIndex(name) !== -1 || Reflect.has(target, name);
      },
      ownKeys() {
        return LIMIT_NAMES.slice();
      },
      getOwnPropertyDescriptor(target, name) {
        const id = limitIndex(name);
        if (id === -1) return Reflect.getOwnPropertyDescriptor(target, name);
        return { value: internals.limit.call(db, id), writable: true, enumerable: true, configurable: true };
      }
    }
  );
}

function defineMethods(prototype, methods) {
  for (const key of Reflect.ownKeys(methods)) {
    const descriptor = Object.getOwnPropertyDescriptor(methods, key);
    descriptor.enumerable = false;
    Object.defineProperty(prototype, key, descriptor);
  }
}

internals.setPathHook(path => normalizePath(path, 'path'));

defineMethods(Database.prototype, {
  get limits() {
    let limits = limitsByDatabase.get(this);
    if (limits === undefined) {
      if (!(this instanceof Database)) throw codedError(TypeError, 'ERR_INVALID_THIS', 'Value of "this" must be of type Database');
      limits = createLimits(this);
      limitsByDatabase.set(this, limits);
    }
    return limits;
  },

  createTagStore(maxSize = 1000) {
    if (!this.isOpen) throw codedError(Error, 'ERR_INVALID_STATE', 'database is not open');
    if (!Number.isInteger(maxSize) || maxSize < 0) throw argTypeError('maxSize', 'a non-negative integer');
    return new SQLTagStore(constructToken, this, maxSize);
  },

  createModule(name, options) {
    validateString(name, 'name');
    validateObject(options, 'options');

    const { columns, rows, directOnly = false, useBigIntArguments = false } = options;
    if (!Array.isArray(columns)) throw argTypeError('options.columns', 'an array');
    if (columns.length === 0) throw codedError(TypeError, 'ERR_INVALID_ARG_VALUE', 'The "options.columns" array must not be empty.');
    validateFunction(rows, 'options.rows');
    validateBoolean(directOnly, 'options.directOnly');
    validateBoolean(useBigIntArguments, 'options.useBigIntArguments');

    const hidden = [];
    const definitions = columns.map((column, index) => {
      validateObject(column, `options.columns[${index}]`);
      validateString(column.name, `options.columns[${index}].name`);
      if (!COLUMN_TYPES.has(column.type)) {
        throw codedError(TypeError, 'ERR_INVALID_ARG_VALUE', `The column "type" property must be one of 'INTEGER', 'TEXT', 'REAL', 'BLOB', or 'ANY'.`);
      }
      if (column.hidden !== undefined) validateBoolean(column.hidden, `options.columns[${index}].hidden`);

      hidden.push(column.hidden === true);
      return `"${column.name.replaceAll('"', '""')}" ${column.type}${column.hidden ? ' HIDDEN' : ''}`;
    });

    const open = (...args) => {
      const iterator = rows(...args)?.[Symbol.iterator]?.();
      if (iterator === undefined || iterator === null) {
        throw codedError(TypeError, 'ERR_INVALID_ARG_TYPE', 'The "options.rows" function must return an iterable.');
      }
      return () => {
        const step = iterator.next();
        return step.done ? undefined : step.value;
      };
    };

    const flags = (useBigIntArguments ? 1 : 0) | (directOnly ? 2 : 0);
    internals.createModule.call(this, name, `CREATE TABLE x(${definitions.join(', ')})`, hidden, open, flags);
  },

  [Symbol.dispose]() {
    if (this.isOpen) this.close();
  }
});

class StatementIterator extends Iterator {
  #statement;
  #token;
  #done = false;

  constructor(token, statement, generation) {
    if (token !== constructToken) throw codedError(Error, 'ERR_ILLEGAL_CONSTRUCTOR', 'Illegal constructor');
    super();
    this.#statement = statement;
    this.#token = generation;
  }

  next() {
    if (this.#done) return { __proto__: null, done: true, value: null };

    let row;
    try {
      row = internals.step.call(this.#statement, this.#token);
    } catch (error) {
      this.#done = true;
      throw error;
    }

    if (row === undefined) {
      this.#done = true;
      return { __proto__: null, done: true, value: null };
    }
    return { __proto__: null, done: false, value: row };
  }

  return() {
    if (!this.#done) {
      this.#done = true;
      internals.end.call(this.#statement, this.#token);
    }
    return { __proto__: null, done: true, value: null };
  }
}

defineMethods(Statement.prototype, {
  iterate(...params) {
    return new StatementIterator(constructToken, this, internals.begin.apply(this, params));
  },

  [Symbol.dispose]() {
    if (!internals.isFinalized.call(this)) this.close();
  }
});

defineMethods(Session.prototype, {
  [Symbol.dispose]() {
    if (!internals.isClosed.call(this)) this.close();
  }
});

class SQLTagStore {
  #db;
  #capacity;
  #cache = new Map();

  constructor(token, db, capacity) {
    if (token !== constructToken) throw codedError(Error, 'ERR_ILLEGAL_CONSTRUCTOR', 'Illegal constructor');
    this.#db = db;
    this.#capacity = capacity;
  }

  #prepare(strings, params) {
    if (!Array.isArray(strings)) throw argTypeError('stringElements', 'an array of strings');

    const first = params[0];
    if (params.length > 0 && first !== null && typeof first === 'object' && !isBinary(first)) {
      throw codedError(TypeError, 'ERR_INVALID_ARG_TYPE', 'Provided value cannot be bound to SQLite parameter 1.');
    }

    const sql = strings.join('?');
    const cache = this.#cache;

    let statement = cache.get(sql);
    if (statement !== undefined) {
      cache.delete(sql);
      if (!internals.isFinalized.call(statement)) {
        cache.set(sql, statement);
        return statement;
      }
    }

    statement = this.#db.prepare(sql);
    if (this.#capacity === 0) return statement;

    cache.set(sql, statement);
    if (cache.size > this.#capacity) cache.delete(cache.keys().next().value);
    return statement;
  }

  run(strings, ...params) {
    return this.#prepare(strings, params).run(...params);
  }

  get(strings, ...params) {
    return this.#prepare(strings, params).get(...params);
  }

  all(strings, ...params) {
    return this.#prepare(strings, params).all(...params);
  }

  iterate(strings, ...params) {
    return this.#prepare(strings, params).iterate(...params);
  }

  clear() {
    this.#cache.clear();
  }

  get size() {
    return this.#cache.size;
  }

  get capacity() {
    return this.#capacity;
  }

  get db() {
    return this.#db;
  }
}

function backup(sourceDb, path, options = {}) {
  if (!(sourceDb instanceof Database)) throw argTypeError('sourceDb', 'an object');
  const target = normalizePath(path, 'path');
  validateObject(options, 'options');

  const { source = 'main', target: targetName = 'main', rate = 100, progress } = options;
  validateString(source, 'options.source');
  validateString(targetName, 'options.target');
  if (!Number.isInteger(rate)) throw argTypeError('options.rate', 'an integer');
  if (progress !== undefined) validateFunction(progress, 'options.progress');

  let handle;
  try {
    handle = internals.backupInit(sourceDb, target, source, targetName);
  } catch (error) {
    return Promise.reject(error);
  }

  return new Promise((resolve, reject) => {
    const tick = () => {
      let state;
      try {
        state = internals.backupStep(handle, rate);
      } catch (error) {
        reject(error);
        return;
      }

      if (state.done) {
        resolve(state.totalPages);
        return;
      }

      try {
        if (progress !== undefined) progress({ totalPages: state.totalPages, remainingPages: state.remainingPages });
      } catch (error) {
        internals.backupAbort(handle);
        reject(error);
        return;
      }

      setImmediate(tick);
    };

    setImmediate(tick);
  });
}

const queryChannel = channel('sqlite.db.query');

function publishQuery(sql, database, duration) {
  queryChannel.publish({ sql, database, duration });
}

function syncQueryHook() {
  internals.setQueryHook(queryChannel.hasSubscribers ? publishQuery : undefined);
}

for (const method of ['subscribe', 'unsubscribe', 'bindStore', 'unbindStore']) {
  const original = queryChannel[method];
  Object.defineProperty(queryChannel, method, {
    configurable: true,
    writable: true,
    value(...args) {
      const result = original.apply(this, args);
      syncQueryHook();
      return result;
    }
  });
}

syncQueryHook();

const constants = {
  SQLITE_CHANGESET_OMIT: 0,
  SQLITE_CHANGESET_REPLACE: 1,
  SQLITE_CHANGESET_ABORT: 2,
  SQLITE_CHANGESET_DATA: 1,
  SQLITE_CHANGESET_NOTFOUND: 2,
  SQLITE_CHANGESET_CONFLICT: 3,
  SQLITE_CHANGESET_CONSTRAINT: 4,
  SQLITE_CHANGESET_FOREIGN_KEY: 5,
  SQLITE_OK: 0,
  SQLITE_DENY: 1,
  SQLITE_IGNORE: 2,
  SQLITE_CREATE_INDEX: 1,
  SQLITE_CREATE_TABLE: 2,
  SQLITE_CREATE_TEMP_INDEX: 3,
  SQLITE_CREATE_TEMP_TABLE: 4,
  SQLITE_CREATE_TEMP_TRIGGER: 5,
  SQLITE_CREATE_TEMP_VIEW: 6,
  SQLITE_CREATE_TRIGGER: 7,
  SQLITE_CREATE_VIEW: 8,
  SQLITE_DELETE: 9,
  SQLITE_DROP_INDEX: 10,
  SQLITE_DROP_TABLE: 11,
  SQLITE_DROP_TEMP_INDEX: 12,
  SQLITE_DROP_TEMP_TABLE: 13,
  SQLITE_DROP_TEMP_TRIGGER: 14,
  SQLITE_DROP_TEMP_VIEW: 15,
  SQLITE_DROP_TRIGGER: 16,
  SQLITE_DROP_VIEW: 17,
  SQLITE_INSERT: 18,
  SQLITE_PRAGMA: 19,
  SQLITE_READ: 20,
  SQLITE_SELECT: 21,
  SQLITE_TRANSACTION: 22,
  SQLITE_UPDATE: 23,
  SQLITE_ATTACH: 24,
  SQLITE_DETACH: 25,
  SQLITE_ALTER_TABLE: 26,
  SQLITE_REINDEX: 27,
  SQLITE_ANALYZE: 28,
  SQLITE_CREATE_VTABLE: 29,
  SQLITE_DROP_VTABLE: 30,
  SQLITE_FUNCTION: 31,
  SQLITE_SAVEPOINT: 32,
  SQLITE_COPY: 0,
  SQLITE_RECURSIVE: 33
};

export { Database, Database as DatabaseSync, Statement, Statement as StatementSync, Session, constants, backup };
export default { Database, DatabaseSync: Database, Statement, StatementSync: Statement, Session, constants, backup };
