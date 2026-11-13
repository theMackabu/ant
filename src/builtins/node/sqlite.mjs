import binding from 'ant:internal/sqlite';
import { fileURLToPath } from 'node:url';
import { channel } from 'node:diagnostics_channel';

const { Database, Statement, Session, internals } = binding;

const MAX_LIMIT = 0x7fffffff;
const COLUMN_TYPES = ['INTEGER', 'TEXT', 'REAL', 'BLOB', 'ANY'];
const EXPECTED = { string: 'a string', boolean: 'a boolean', function: 'a function', object: 'an object' };

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

const AUTHORIZER_ACTIONS = [
  'COPY',
  'CREATE_INDEX',
  'CREATE_TABLE',
  'CREATE_TEMP_INDEX',
  'CREATE_TEMP_TABLE',
  'CREATE_TEMP_TRIGGER',
  'CREATE_TEMP_VIEW',
  'CREATE_TRIGGER',
  'CREATE_VIEW',
  'DELETE',
  'DROP_INDEX',
  'DROP_TABLE',
  'DROP_TEMP_INDEX',
  'DROP_TEMP_TABLE',
  'DROP_TEMP_TRIGGER',
  'DROP_TEMP_VIEW',
  'DROP_TRIGGER',
  'DROP_VIEW',
  'INSERT',
  'PRAGMA',
  'READ',
  'SELECT',
  'TRANSACTION',
  'UPDATE',
  'ATTACH',
  'DETACH',
  'ALTER_TABLE',
  'REINDEX',
  'ANALYZE',
  'CREATE_VTABLE',
  'DROP_VTABLE',
  'FUNCTION',
  'SAVEPOINT',
  'RECURSIVE'
];

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
  SQLITE_IGNORE: 2
};

AUTHORIZER_ACTIONS.forEach((name, code) => {
  constants[`SQLITE_${name}`] = code;
});

function failure(Type, code, message) {
  const error = new Type(message);
  error.code = code;
  return error;
}

const invalidType = (name, expected) => invalidArgument(`The "${name}" argument must be ${expected}.`);
const invalidArgument = message => failure(TypeError, 'ERR_INVALID_ARG_TYPE', message);
const invalidValue = message => failure(TypeError, 'ERR_INVALID_ARG_VALUE', message);
const invalidState = message => failure(Error, 'ERR_INVALID_STATE', message);
const invalidThis = name => failure(TypeError, 'ERR_INVALID_THIS', `Value of "this" must be of type ${name}`);
const illegalConstructor = () => failure(Error, 'ERR_ILLEGAL_CONSTRUCTOR', 'Illegal constructor');

function expect(type, value, name) {
  if (typeof value !== type || value === null) throw invalidType(name, EXPECTED[type]);
}

function isBinary(value) {
  return ArrayBuffer.isView(value) || value instanceof ArrayBuffer || value instanceof SharedArrayBuffer;
}

function toPath(path) {
  if (path instanceof Uint8Array) path = new TextDecoder().decode(path);
  else if (path instanceof URL) path = fileURLToPath(path);

  if (typeof path === 'string' && !path.includes('\0')) return path;
  throw invalidType('path', 'a string, Uint8Array, or URL without null bytes');
}

function extend(Class, Mixin) {
  const { constructor, ...members } = Object.getOwnPropertyDescriptors(Mixin.prototype);
  Object.defineProperties(Class.prototype, members);
}

const construct = Symbol('construct');
const limitsOf = new WeakMap();

function createLimits(db) {
  const values = Object.fromEntries(LIMIT_NAMES.map(name => [name, undefined]));

  const view = new Proxy(values, {
    get(target, name, receiver) {
      const id = LIMIT_NAMES.indexOf(name);
      return id === -1 ? Reflect.get(target, name, receiver) : (target[name] = internals.limit.call(db, id));
    },

    set(target, name, value, receiver) {
      const id = LIMIT_NAMES.indexOf(name);
      if (id === -1) return Reflect.set(target, name, value, receiver);

      if (typeof value !== 'number') throw invalidType(`limits.${name}`, 'a number');
      if (value !== Infinity && !(Number.isInteger(value) && value >= 0)) {
        throw failure(RangeError, 'ERR_OUT_OF_RANGE', 'Limit value must be non-negative.');
      }

      target[name] = internals.limit.call(db, id, Math.min(value, MAX_LIMIT));
      return true;
    }
  });

  return { values, view };
}

function refreshLimits(db, values) {
  if (!db.isOpen) return;
  for (let id = 0; id < LIMIT_NAMES.length; id++) values[LIMIT_NAMES[id]] = internals.limit.call(db, id);
}

function columnDefinition(column, index) {
  const label = `options.columns[${index}]`;
  expect('object', column, label);
  expect('string', column.name, `${label}.name`);
  if (column.hidden !== undefined) expect('boolean', column.hidden, `${label}.hidden`);

  if (!COLUMN_TYPES.includes(column.type)) {
    throw invalidValue(`The column "type" property must be one of 'INTEGER', 'TEXT', 'REAL', 'BLOB', or 'ANY'.`);
  }

  const name = column.name.replaceAll('"', '""');
  return `"${name}" ${column.type}${column.hidden ? ' HIDDEN' : ''}`;
}

function rowSource(rows) {
  return (...args) => {
    const iterator = rows(...args)?.[Symbol.iterator]?.();
    if (iterator == null) throw invalidType('options.rows', 'a function returning an iterable');

    return () => {
      const step = iterator.next();
      return step.done ? undefined : step.value;
    };
  };
}

extend(Database, class {
  get limits() {
    let limits = limitsOf.get(this);
    if (limits === undefined) {
      if (!(this instanceof Database)) throw invalidThis('Database');
      limitsOf.set(this, (limits = createLimits(this)));
    }

    refreshLimits(this, limits.values);
    return limits.view;
  }

  createTagStore(maxSize = 1000) {
    if (!this.isOpen) throw invalidState('database is not open');
    if (!(Number.isInteger(maxSize) && maxSize >= 0)) throw invalidType('maxSize', 'a non-negative integer');
    return new SQLTagStore(construct, this, maxSize);
  }

  createModule(name, options) {
    expect('string', name, 'name');
    expect('object', options, 'options');

    const { columns, rows, directOnly = false, useBigIntArguments = false } = options;
    if (!Array.isArray(columns)) throw invalidType('options.columns', 'an array');
    if (columns.length === 0) throw invalidValue('The "options.columns" array must not be empty.');
    expect('function', rows, 'options.rows');
    expect('boolean', directOnly, 'options.directOnly');
    expect('boolean', useBigIntArguments, 'options.useBigIntArguments');

    const schema = `CREATE TABLE x(${columns.map(columnDefinition).join(', ')})`;
    const hidden = columns.map(column => column.hidden === true);
    const flags = (useBigIntArguments ? 1 : 0) | (directOnly ? 2 : 0);

    internals.createModule.call(this, name, schema, hidden, rowSource(rows), flags);
  }

  [Symbol.dispose]() {
    if (this.isOpen) this.close();
  }
});

extend(Statement, class {
  iterate(...params) {
    return new StatementIterator(construct, this, internals.begin.apply(this, params));
  }

  [Symbol.dispose]() {
    if (!internals.isFinalized.call(this)) this.close();
  }
});

extend(Session, class {
  [Symbol.dispose]() {
    if (!internals.isClosed.call(this)) this.close();
  }
});

class StatementIterator extends Iterator {
  #statement;
  #generation;
  #done = false;

  constructor(token, statement, generation) {
    if (token !== construct) throw illegalConstructor();
    super();
    this.#statement = statement;
    this.#generation = generation;
  }

  next() {
    if (this.#done) return { __proto__: null, done: true, value: null };

    let row;
    try {
      row = internals.step.call(this.#statement, this.#generation);
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
      internals.end.call(this.#statement, this.#generation);
    }
    return { __proto__: null, done: true, value: null };
  }
}

class SQLTagStore {
  #db;
  #capacity;
  #cache = new Map();

  constructor(token, db, capacity) {
    if (token !== construct) throw illegalConstructor();
    this.#db = db;
    this.#capacity = capacity;
  }

  #statement(strings, params) {
    if (!Array.isArray(strings)) throw invalidType('stringElements', 'an array of strings');

    const first = params[0];
    if (params.length > 0 && first !== null && typeof first === 'object' && !isBinary(first)) {
      throw invalidArgument('Provided value cannot be bound to SQLite parameter 1.');
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
    return this.#statement(strings, params).run(...params);
  }

  get(strings, ...params) {
    return this.#statement(strings, params).get(...params);
  }

  all(strings, ...params) {
    return this.#statement(strings, params).all(...params);
  }

  iterate(strings, ...params) {
    return this.#statement(strings, params).iterate(...params);
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
  if (!(sourceDb instanceof Database)) throw invalidType('sourceDb', 'an object');
  path = toPath(path);
  expect('object', options, 'options');

  const { source = 'main', target = 'main', rate = 100, progress } = options;
  expect('string', source, 'options.source');
  expect('string', target, 'options.target');
  if (!Number.isInteger(rate)) throw invalidType('options.rate', 'an integer');
  if (progress !== undefined) expect('function', progress, 'options.progress');

  return new Promise((resolve, reject) => {
    const handle = internals.backupInit(sourceDb, path, source, target);

    const step = () => {
      try {
        const { done, totalPages, remainingPages } = internals.backupStep(handle, rate);
        if (done) return resolve(totalPages);

        if (progress !== undefined) progress({ totalPages, remainingPages });
        setImmediate(step);
      } catch (error) {
        internals.backupAbort(handle);
        reject(error);
      }
    };

    setImmediate(step);
  });
}

const queries = channel('sqlite.db.query');
const publishQuery = (sql, database, duration) => queries.publish({ sql, database, duration });
const syncQueryHook = () => internals.setQueryHook(queries.hasSubscribers ? publishQuery : undefined);

for (const name of ['subscribe', 'unsubscribe', 'bindStore', 'unbindStore']) {
  const method = queries[name];
  queries[name] = function (...args) {
    const result = method.apply(this, args);
    syncQueryHook();
    return result;
  };
}

internals.setPathHook(toPath);
syncQueryHook();

export { Database, Database as DatabaseSync, Statement, Statement as StatementSync, Session, constants, backup };
export default { Database, DatabaseSync: Database, Statement, StatementSync: Statement, Session, constants, backup };
