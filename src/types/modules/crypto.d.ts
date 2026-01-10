declare module 'crypto' {
  interface WebCrypto {
    random(): number;
    randomBytes(length: number): number[];
    randomUUID(): string;
    randomUUIDv7(): string;
    getRandomValues<T extends ArrayBufferView>(array: T): T;
  }

  const webcrypto: WebCrypto;

  function randomBytes(length: number): number[];
  function randomUUID(): string;
  function getRandomValues<T extends ArrayBufferView>(array: T): T;
}
