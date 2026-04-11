declare namespace utilTypes {
  function isAnyArrayBuffer(value: unknown): boolean;
  function isArrayBuffer(value: unknown): boolean;
  function isArgumentsObject(value: unknown): boolean;
  function isArrayBufferView(value: unknown): boolean;
  function isAsyncFunction(value: unknown): boolean;
  function isBigInt64Array(value: unknown): boolean;
  function isBigIntObject(value: unknown): boolean;
  function isBigUint64Array(value: unknown): boolean;
  function isBooleanObject(value: unknown): boolean;
  function isBoxedPrimitive(value: unknown): boolean;
  function isDataView(value: unknown): boolean;
  function isDate(value: unknown): boolean;
  function isFloat16Array(value: unknown): boolean;
  function isFloat32Array(value: unknown): boolean;
  function isFloat64Array(value: unknown): boolean;
  function isGeneratorFunction(value: unknown): boolean;
  function isGeneratorObject(value: unknown): boolean;
  function isInt8Array(value: unknown): boolean;
  function isInt16Array(value: unknown): boolean;
  function isInt32Array(value: unknown): boolean;
  function isMap(value: unknown): boolean;
  function isMapIterator(value: unknown): boolean;
  function isModuleNamespaceObject(value: unknown): boolean;
  function isNativeError(value: unknown): boolean;
  function isNumberObject(value: unknown): boolean;
  function isPromise(value: unknown): boolean;
  function isProxy(value: unknown): boolean;
  function isRegExp(value: unknown): boolean;
  function isSet(value: unknown): boolean;
  function isSetIterator(value: unknown): boolean;
  function isSharedArrayBuffer(value: unknown): boolean;
  function isStringObject(value: unknown): boolean;
  function isSymbolObject(value: unknown): boolean;
  function isTypedArray(value: unknown): boolean;
  function isUint8Array(value: unknown): boolean;
  function isUint8ClampedArray(value: unknown): boolean;
  function isUint16Array(value: unknown): boolean;
  function isUint32Array(value: unknown): boolean;
  function isWeakMap(value: unknown): boolean;
  function isWeakSet(value: unknown): boolean;
}

declare module 'util' {
  type StyleTextFormat = string | string[];

  interface StyleTextOptions {
    validateStream?: boolean;
    stream?: unknown;
  }

  function format(format?: unknown, ...args: unknown[]): string;
  function formatWithOptions(inspectOptions: unknown, format?: unknown, ...args: unknown[]): string;
  function inspect(value: unknown, options?: unknown): string;
  function inherits(ctor: (...args: unknown[]) => unknown, superCtor: (...args: unknown[]) => unknown): void;
  function isDeepStrictEqual(a: unknown, b: unknown): boolean;
  function parseArgs(config: {
    args?: string[];
    options?: Record<string, {
      type?: 'boolean' | 'string';
      short?: string;
      multiple?: boolean;
      default?: unknown;
    }>;
    strict?: boolean;
    allowPositionals?: boolean;
  }): { values: Record<string, unknown>; positionals: string[] };
  function parseEnv(content: string): Record<string, string>;
  function promisify(fn: (...args: unknown[]) => unknown): (...args: unknown[]) => Promise<unknown>;
  function callbackify(fn: (...args: unknown[]) => Promise<unknown>): (...args: unknown[]) => void;
  function aborted(signal: AbortSignal, resource: object): Promise<void>;
  function stripVTControlCharacters(str: string): string;
  function styleText(format: StyleTextFormat, text: string, options?: StyleTextOptions): string;

  const types: typeof utilTypes;
}

declare module 'ant:util' {
  export * from 'util';
}

declare module 'node:util' {
  export * from 'util';
}

declare module 'util/types' {
  export = utilTypes;
}

declare module 'node:util/types' {
  export = utilTypes;
}
