declare module 'zlib' {
  type Input = string | ArrayBuffer | ArrayBufferView;
  type Callback = (error: Error | null, result: Uint8Array) => void;

  interface ZlibOptions {
    chunkSize?: number;
    flush?: number;
    finishFlush?: number;
    level?: number;
    memLevel?: number;
    strategy?: number;
    windowBits?: number;
  }

  interface BrotliOptions {
    chunkSize?: number;
    flush?: number;
    finishFlush?: number;
    params?: Record<number, number>;
  }

  interface ZlibBase {
    readonly bytesWritten: number;
    readable: boolean;
    writable: boolean;
    write(chunk?: Input, encoding?: string | ((error?: Error | null) => void), callback?: (error?: Error | null) => void): boolean;
    end(chunk?: Input, encoding?: string | (() => void), callback?: () => void): this;
    flush(kind?: number | (() => void), callback?: () => void): this;
    close(callback?: () => void): this;
    params(level: number, strategy: number, callback?: () => void): this;
    reset(): this;
    destroy(error?: Error): this;
    pause(): this;
    resume(): this;
    pipe<T>(destination: T, options?: { end?: boolean }): T;
    unpipe(destination?: unknown): this;
    on(event: 'data', listener: (chunk: Uint8Array) => void): this;
    on(event: 'end' | 'finish' | 'close', listener: () => void): this;
    on(event: 'error', listener: (error: Error) => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
    once(event: string, listener: (...args: unknown[]) => void): this;
  }

  class Gzip implements ZlibBase {
    constructor(options?: ZlibOptions);
    readonly bytesWritten: number;
    readable: boolean;
    writable: boolean;
    write: ZlibBase['write'];
    end: ZlibBase['end'];
    flush: ZlibBase['flush'];
    close: ZlibBase['close'];
    params: ZlibBase['params'];
    reset: ZlibBase['reset'];
    destroy: ZlibBase['destroy'];
    pause: ZlibBase['pause'];
    resume: ZlibBase['resume'];
    pipe: ZlibBase['pipe'];
    unpipe: ZlibBase['unpipe'];
    on: ZlibBase['on'];
    once: ZlibBase['once'];
  }
  class Gunzip extends Gzip {}
  class Deflate extends Gzip {}
  class Inflate extends Gzip {}
  class DeflateRaw extends Gzip {}
  class InflateRaw extends Gzip {}
  class Unzip extends Gzip {}
  class BrotliCompress extends Gzip {}
  class BrotliDecompress extends Gzip {}

  const constants: Record<string, number>;
  const codes: Record<string, string | number>;

  function createGzip(options?: ZlibOptions): Gzip;
  function createGunzip(options?: ZlibOptions): Gunzip;
  function createDeflate(options?: ZlibOptions): Deflate;
  function createInflate(options?: ZlibOptions): Inflate;
  function createDeflateRaw(options?: ZlibOptions): DeflateRaw;
  function createInflateRaw(options?: ZlibOptions): InflateRaw;
  function createUnzip(options?: ZlibOptions): Unzip;
  function createBrotliCompress(options?: BrotliOptions): BrotliCompress;
  function createBrotliDecompress(options?: BrotliOptions): BrotliDecompress;

  function gzip(buffer: Input, callback: Callback): void;
  function gzip(buffer: Input, options: ZlibOptions, callback: Callback): void;
  function gzipSync(buffer: Input, options?: ZlibOptions): Uint8Array;
  function gunzip(buffer: Input, callback: Callback): void;
  function gunzip(buffer: Input, options: ZlibOptions, callback: Callback): void;
  function gunzipSync(buffer: Input, options?: ZlibOptions): Uint8Array;
  function deflate(buffer: Input, callback: Callback): void;
  function deflate(buffer: Input, options: ZlibOptions, callback: Callback): void;
  function deflateSync(buffer: Input, options?: ZlibOptions): Uint8Array;
  function inflate(buffer: Input, callback: Callback): void;
  function inflate(buffer: Input, options: ZlibOptions, callback: Callback): void;
  function inflateSync(buffer: Input, options?: ZlibOptions): Uint8Array;
  function deflateRaw(buffer: Input, callback: Callback): void;
  function deflateRaw(buffer: Input, options: ZlibOptions, callback: Callback): void;
  function deflateRawSync(buffer: Input, options?: ZlibOptions): Uint8Array;
  function inflateRaw(buffer: Input, callback: Callback): void;
  function inflateRaw(buffer: Input, options: ZlibOptions, callback: Callback): void;
  function inflateRawSync(buffer: Input, options?: ZlibOptions): Uint8Array;
  function unzip(buffer: Input, callback: Callback): void;
  function unzip(buffer: Input, options: ZlibOptions, callback: Callback): void;
  function unzipSync(buffer: Input, options?: ZlibOptions): Uint8Array;
  function brotliCompress(buffer: Input, callback: Callback): void;
  function brotliCompress(buffer: Input, options: BrotliOptions, callback: Callback): void;
  function brotliCompressSync(buffer: Input, options?: BrotliOptions): Uint8Array;
  function brotliDecompress(buffer: Input, callback: Callback): void;
  function brotliDecompress(buffer: Input, options: BrotliOptions, callback: Callback): void;
  function brotliDecompressSync(buffer: Input, options?: BrotliOptions): Uint8Array;
  function crc32(data: Input, value?: number): number;

  const zlib: {
    constants: typeof constants;
    codes: typeof codes;
    Gzip: typeof Gzip;
    Gunzip: typeof Gunzip;
    Deflate: typeof Deflate;
    Inflate: typeof Inflate;
    DeflateRaw: typeof DeflateRaw;
    InflateRaw: typeof InflateRaw;
    Unzip: typeof Unzip;
    BrotliCompress: typeof BrotliCompress;
    BrotliDecompress: typeof BrotliDecompress;
    createGzip: typeof createGzip;
    createGunzip: typeof createGunzip;
    createDeflate: typeof createDeflate;
    createInflate: typeof createInflate;
    createDeflateRaw: typeof createDeflateRaw;
    createInflateRaw: typeof createInflateRaw;
    createUnzip: typeof createUnzip;
    createBrotliCompress: typeof createBrotliCompress;
    createBrotliDecompress: typeof createBrotliDecompress;
    gzip: typeof gzip;
    gzipSync: typeof gzipSync;
    gunzip: typeof gunzip;
    gunzipSync: typeof gunzipSync;
    deflate: typeof deflate;
    deflateSync: typeof deflateSync;
    inflate: typeof inflate;
    inflateSync: typeof inflateSync;
    deflateRaw: typeof deflateRaw;
    deflateRawSync: typeof deflateRawSync;
    inflateRaw: typeof inflateRaw;
    inflateRawSync: typeof inflateRawSync;
    unzip: typeof unzip;
    unzipSync: typeof unzipSync;
    brotliCompress: typeof brotliCompress;
    brotliCompressSync: typeof brotliCompressSync;
    brotliDecompress: typeof brotliDecompress;
    brotliDecompressSync: typeof brotliDecompressSync;
    crc32: typeof crc32;
    [name: string]: unknown;
  };

  export default zlib;
  export {
    constants,
    codes,
    Gzip,
    Gunzip,
    Deflate,
    Inflate,
    DeflateRaw,
    InflateRaw,
    Unzip,
    BrotliCompress,
    BrotliDecompress,
    createGzip,
    createGunzip,
    createDeflate,
    createInflate,
    createDeflateRaw,
    createInflateRaw,
    createUnzip,
    createBrotliCompress,
    createBrotliDecompress,
    gzip,
    gzipSync,
    gunzip,
    gunzipSync,
    deflate,
    deflateSync,
    inflate,
    inflateSync,
    deflateRaw,
    deflateRawSync,
    inflateRaw,
    inflateRawSync,
    unzip,
    unzipSync,
    brotliCompress,
    brotliCompressSync,
    brotliDecompress,
    brotliDecompressSync,
    crc32
  };
}

declare module 'ant:zlib' {
  export * from 'zlib';
  export { default } from 'zlib';
}

declare module 'node:zlib' {
  export * from 'zlib';
  export { default } from 'zlib';
}
