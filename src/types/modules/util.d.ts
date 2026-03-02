declare module 'util' {
  type StyleTextFormat = string | string[];

  interface StyleTextOptions {
    validateStream?: boolean;
    stream?: unknown;
  }

  function format(format?: unknown, ...args: unknown[]): string;
  function formatWithOptions(
    inspectOptions: unknown,
    format?: unknown,
    ...args: unknown[]
  ): string;
  function inspect(value: unknown, options?: unknown): string;
  function promisify(
    fn: (...args: unknown[]) => unknown
  ): (...args: unknown[]) => Promise<unknown>;
  function stripVTControlCharacters(str: string): string;
  function styleText(
    format: StyleTextFormat,
    text: string,
    options?: StyleTextOptions
  ): string;
}

declare module 'ant:util' {
  export * from 'util';
}

declare module 'node:util' {
  export * from 'util';
}
