declare module 'crypto' {
  type BufferSource = ArrayBuffer | ArrayBufferView;

  interface SubtleCrypto {
    digest(algorithm: string | { name: string }, data: BufferSource): Promise<ArrayBuffer>;
    deriveBits(algorithm: string | { name: string }, baseKey: any, length: number): Promise<ArrayBuffer>;
    timingSafeEqual(a: BufferSource, b: BufferSource): boolean;
  }

  interface WebCrypto {
    random(): number;
    randomBytes(length: number): number[];
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
  
  function randomBytes(length: number): number[];
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
