interface BufferStatic {
  from(data: string | ArrayLike<number>): Uint8Array & BufferMethods;
  alloc(size: number): Uint8Array & BufferMethods;
  allocUnsafe(size: number): Uint8Array & BufferMethods;
}

interface BufferMethods {
  toString(encoding?: 'utf8' | 'base64' | 'hex'): string;
  toBase64(): string;
  write(string: string, offset?: number, length?: number): number;
}

declare const Buffer: BufferStatic;
