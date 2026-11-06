declare module 'crypto' {
  type BufferSource = ArrayBuffer | ArrayBufferView;

  interface SubtleCrypto {
    digest(algorithm: string | { name: string }, data: BufferSource): Promise<ArrayBuffer>;
    deriveBits(algorithm: string | { name: string }, baseKey: any, length: number): Promise<ArrayBuffer>;
    timingSafeEqual(a: BufferSource, b: BufferSource): boolean;
  }

  interface WebCrypto {
    random(): number;
    randomBytes(size: number): Buffer;
    randomBytes(size: number, callback: (err: Error | null, buf: Buffer) => void): void;
    randomUUID(): string;
    randomUUIDv7(): string;
    getRandomValues<T extends ArrayBufferView>(array: T): T;
    subtle: SubtleCrypto;
  }

  const webcrypto: WebCrypto;

  function createHash(algorithm: string): {
    update(data: string | BufferSource, inputEncoding?: string): any;
    digest(encoding?: string): string | Uint8Array;
  };
  
  type RandomFillCallback<T> = (err: Error | null, buf: T) => void;

  function randomBytes(size: number): Buffer;
  function randomBytes(size: number, callback: (err: Error | null, buf: Buffer) => void): void;
  const pseudoRandomBytes: typeof randomBytes;
  const prng: typeof randomBytes;
  const rng: typeof randomBytes;
  function randomFill<T extends BufferSource | SharedArrayBuffer>(buf: T, callback: RandomFillCallback<T>): void;
  function randomFill<T extends BufferSource | SharedArrayBuffer>(buf: T, offset: number, callback: RandomFillCallback<T>): void;
  function randomFill<T extends BufferSource | SharedArrayBuffer>(buf: T, offset: number, size: number, callback: RandomFillCallback<T>): void;
  function randomFillSync<T extends BufferSource | SharedArrayBuffer>(buf: T, offset?: number, size?: number): T;
  function randomInt(max: number): number;
  function randomInt(min: number, max: number): number;
  function randomInt(max: number, callback: (err: undefined, value: number) => void): void;
  function randomInt(min: number, max: number, callback: (err: undefined, value: number) => void): void;
  function randomUUID(): string;
  function getRandomValues<T extends ArrayBufferView>(array: T): T;
  function getCurves(): string[];
  function getHashes(): string[];
  function timingSafeEqual(a: BufferSource, b: BufferSource): boolean;
}

declare module 'ant:crypto' {
  export * from 'crypto';
}

declare module 'node:crypto' {
  export * from 'crypto';
}
