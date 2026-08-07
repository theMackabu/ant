export interface DatabaseOptions {
  readonly?: boolean;
  create?: boolean;
  readwrite?: boolean;
  strict?: boolean;
  safeIntegers?: boolean;
}

export interface Changes {
  changes: number | bigint;
  lastInsertRowid: number | bigint;
}

export type SQLValue = string | number | bigint | boolean | null | undefined | Uint8Array | ArrayBuffer;
export type BindParams = SQLValue | Record<string, SQLValue>;

export declare class Statement<Row = Record<string, unknown>> {
  get(...params: BindParams[]): Row | null;
  all(...params: BindParams[]): Row[];
  values(...params: BindParams[]): unknown[][];
  run(...params: BindParams[]): Changes;
  iterate(...params: BindParams[]): IterableIterator<Row>;
  [Symbol.iterator](): IterableIterator<Row>;
  as<T>(Class: new (...args: never[]) => T): Statement<T>;
  safeIntegers(enabled?: boolean): this;
  finalize(): void;
  toString(): string;
  readonly columnNames: string[];
  /** Not supported: throws. node:sqlite does not expose bind parameter counts. */
  readonly paramsCount: number;
  /** Not supported: throws. */
  readonly native: never;
}

export declare class Database {
  constructor(filename?: string, options?: number | DatabaseOptions);
  readonly filename: string;
  readonly strict: boolean;
  readonly inTransaction: boolean;

  prepare<Row = Record<string, unknown>>(sql: string, ...defaultParams: BindParams[]): Statement<Row>;
  query<Row = Record<string, unknown>>(sql: string): Statement<Row>;
  run(sql: string, ...params: BindParams[]): Changes;
  exec(sql: string, ...params: BindParams[]): Changes;
  transaction<Args extends unknown[], R>(fn: (...args: Args) => R): ((...args: Args) => R) & {
    deferred: (...args: Args) => R;
    immediate: (...args: Args) => R;
    exclusive: (...args: Args) => R;
  };
  close(throwOnError?: boolean): void;

  /** Not supported: throws. */
  serialize(): never;
  /** Not supported: throws. */
  static deserialize(data: unknown): never;
  /** Not supported: throws (SQLITE_OMIT_LOAD_EXTENSION). */
  loadExtension(path: string): never;
  /** Not supported: throws. */
  fileControl(op: number, value?: unknown): never;
}

export declare const constants: {
  SQLITE_OPEN_READONLY: number;
  SQLITE_OPEN_READWRITE: number;
  SQLITE_OPEN_CREATE: number;
  SQLITE_OPEN_URI: number;
  SQLITE_OPEN_MEMORY: number;
};

export default Database;
