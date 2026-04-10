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

declare module 'buffer' {
  const Buffer: BufferStatic;
  const constants: {
    MAX_LENGTH: number;
    MAX_STRING_LENGTH: number;
  };
  const kMaxLength: number;
  const kStringMaxLength: number;
  const INSPECT_MAX_BYTES: number;
  function atob(data: string): string;
  function btoa(data: string): string;
}

declare module 'node:buffer' {
  export * from 'buffer';
}
