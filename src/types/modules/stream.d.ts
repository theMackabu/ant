declare module 'stream' {
  class Stream {
    pipe<T extends Writable>(destination: T, options?: { end?: boolean }): T;
    unpipe(destination?: Writable): this;
    pause(): this;
    resume(): this;
    isPaused(): boolean;
    destroy(error?: Error): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
    once(event: string, listener: (...args: unknown[]) => void): this;
    off(event: string, listener: (...args: unknown[]) => void): this;
    removeListener(event: string, listener: (...args: unknown[]) => void): this;
    removeAllListeners(event?: string): this;
  }

  class Readable extends Stream {
    constructor(options?: Record<string, unknown>);
    _read(size?: number): void;
    read(size?: number): Uint8Array | string | null;
    push(chunk: Uint8Array | string | null): boolean;
    on(event: 'data', listener: (chunk: Uint8Array | string) => void): this;
    on(event: 'end' | 'close' | 'readable', listener: () => void): this;
    on(event: 'error', listener: (error: Error) => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
    static from(source: unknown, options?: Record<string, unknown>): Readable;
    static fromWeb(source: unknown, options?: Record<string, unknown>): Readable;
  }

  class Writable extends Stream {
    constructor(options?: Record<string, unknown>);
    _write(chunk: Uint8Array | string, encoding: string, callback: (error?: Error | null) => void): void;
    write(chunk: Uint8Array | string, encoding?: string, callback?: (error?: Error | null) => void): boolean;
    end(chunk?: Uint8Array | string, encoding?: string, callback?: (error?: Error | null) => void): this;
    cork(): void;
    uncork(): void;
    on(event: 'drain' | 'finish' | 'close', listener: () => void): this;
    on(event: 'error', listener: (error: Error) => void): this;
    on(event: string, listener: (...args: unknown[]) => void): this;
  }

  class Duplex extends Readable {
    constructor(options?: Record<string, unknown>);
    write(chunk: Uint8Array | string, encoding?: string, callback?: (error?: Error | null) => void): boolean;
    end(chunk?: Uint8Array | string, encoding?: string, callback?: (error?: Error | null) => void): this;
    cork(): void;
    uncork(): void;
  }

  class Transform extends Duplex {
    constructor(options?: Record<string, unknown>);
  }

  class PassThrough extends Transform {
    constructor(options?: Record<string, unknown>);
  }

  function pipeline(...streams: Array<Stream | ((error?: Error | null) => void)>): Stream;
  function finished(stream: Stream, callback?: (error?: Error | null) => void): Stream;
  function getDefaultHighWaterMark(objectMode: boolean): number;
  function setDefaultHighWaterMark(objectMode: boolean, value: number): void;

  const promises: {
    pipeline(...streams: Stream[]): Promise<void>;
    finished(stream: Stream): Promise<void>;
  };

  export default Stream;
  export {
    Stream,
    Readable,
    Writable,
    Duplex,
    Transform,
    PassThrough,
    pipeline,
    finished,
    getDefaultHighWaterMark,
    setDefaultHighWaterMark,
    promises
  };
}

declare module 'ant:stream' {
  export * from 'stream';
  export { default } from 'stream';
}

declare module 'node:stream' {
  export * from 'stream';
  export { default } from 'stream';
}
