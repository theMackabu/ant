export type AntPrimitive = undefined | null | boolean | number | string | bigint;

export type AntValue = AntPrimitive | readonly AntValue[] | { readonly [key: string]: AntValue };

export type AntHostFunction = (...arguments_: AntValue[]) => AntValue;

export interface AntOptions {
  memoryLimit?: number;
  timeout?: number;
  globals?: Record<string, AntValue | AntHostFunction>;
}

export class AntTimeoutError extends Error {}

export class AntDisposedError extends Error {}

export class Ant {
  private constructor();

  static create(options?: AntOptions): Promise<Ant>;

  eval<T = AntValue>(source: string): Promise<T>;

  dispose(): void;

  [Symbol.dispose](): void;
}
