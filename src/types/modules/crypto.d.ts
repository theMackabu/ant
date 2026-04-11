declare module 'crypto' {
  type BufferSource = ArrayBuffer | ArrayBufferView;

  interface SubtleCrypto {
    digest(algorithm: string | { name: string }, data: BufferSource): Promise<ArrayBuffer>;
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
}

declare module 'ant:crypto' {
  export * from 'crypto';
}

declare module 'node:crypto' {
  export * from 'crypto';
}
