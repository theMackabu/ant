declare module 'fs' {
  import type { Readable, Writable } from 'stream';

  interface Stats {
    size: number;
    mode: number;
    uid: number;
    gid: number;
    isFile(): boolean;
    isDirectory(): boolean;
    isSymbolicLink(): boolean;
  }

  type Encoding = 'utf8' | 'utf-8' | 'utf16le' | 'ucs2' | 'ucs-2' | 'latin1' | 'binary' | 'base64' | 'base64url' | 'hex' | 'ascii';
  type WatchEventType = 'rename' | 'change';

  interface FSWatcher {
    close(): this;
    ref(): this;
    unref(): this;
    on(event: 'change', listener: (eventType: WatchEventType, filename?: string) => void): this;
    on(event: 'error', listener: (error: Error) => void): this;
  }

  interface WatchOptions {
    persistent?: boolean;
    recursive?: boolean;
    encoding?: Encoding | 'buffer';
  }

  interface WatchFileOptions {
    persistent?: boolean;
    interval?: number;
  }

  interface ReadStream extends Readable {
    readonly path: string;
    readonly bytesRead: number;
    readonly pending: boolean;
    readonly closed: boolean;
    readonly fd?: number | null;
    close(callback?: () => void): this;
  }

  interface WriteStream extends Writable {
    readonly path: string;
    readonly bytesWritten: number;
    readonly pending: boolean;
    readonly closed: boolean;
    readonly fd?: number | null;
    close(callback?: () => void): this;
  }

  const constants: {
    F_OK: number;
    R_OK: number;
    W_OK: number;
    X_OK: number;
    O_RDONLY: number;
    O_WRONLY: number;
    O_RDWR: number;
    O_CREAT: number;
    O_EXCL: number;
    O_TRUNC: number;
    O_APPEND: number;
  };
  const promises: typeof import('fs/promises');

  function readFile(path: string, encoding: Encoding): Promise<string>;
  function readFile(path: string): Promise<Uint8Array>;
  function readFileSync(path: string, encoding: Encoding | { encoding: Encoding }): string;
  function readFileSync(path: string): Uint8Array;
  function read(
    fd: number,
    buffer: ArrayBufferView,
    offset?: number,
    length?: number,
    position?: number | null,
    callback?: (err: Error | null, bytesRead: number, buffer: ArrayBufferView) => void
  ): Promise<number>;
  function readSync(fd: number, buffer: ArrayBufferView, offset?: number, length?: number, position?: number | null): number;
  function stream(path: string): Promise<string>;
  function createReadStream(
    path: string,
    options?: {
      fd?: number;
      flags?: string | number;
      mode?: number;
      start?: number;
      end?: number;
      autoClose?: boolean;
      emitClose?: boolean;
      highWaterMark?: number;
    }
  ): ReadStream;
  function createWriteStream(
    path: string,
    options?: {
      fd?: number;
      flags?: string | number;
      mode?: number;
      start?: number;
      autoClose?: boolean;
      emitClose?: boolean;
      highWaterMark?: number;
    }
  ): WriteStream;
  function open(path: string, flags?: string, mode?: number): Promise<number>;
  function openSync(path: string, flags?: string, mode?: number): number;
  function close(fd: number): Promise<void>;
  function closeSync(fd: number): void;
  function writeFile(path: string, data: string | ArrayBufferView): Promise<void>;
  function writeFileSync(path: string, data: string | ArrayBufferView): void;
  function write(fd: number, data: string | ArrayBufferView, offset?: number, length?: number, position?: number | null): Promise<number>;
  function writeSync(fd: number, data: string | ArrayBufferView, offset?: number, length?: number, position?: number | null): number;
  function writev(fd: number, buffers: ArrayBufferView[], position?: number): Promise<number>;
  function writevSync(fd: number, buffers: ArrayBufferView[], position?: number): number;
  function appendFileSync(path: string, data: string): void;
  function copyFileSync(src: string, dest: string): void;
  function renameSync(oldPath: string, newPath: string): void;
  function rm(path: string, options?: { recursive?: boolean; force?: boolean }): Promise<void>;
  function unlink(path: string): Promise<void>;
  function rmSync(path: string, options?: { recursive?: boolean; force?: boolean }): void;
  function unlinkSync(path: string): void;
  function mkdir(path: string, options?: { recursive?: boolean; mode?: number }): Promise<void>;
  function mkdirSync(path: string, options?: number | { recursive?: boolean; mode?: number }): void;
  function mkdtemp(prefix: string): Promise<string>;
  function mkdtempSync(prefix: string): string;
  function rmdir(path: string): Promise<void>;
  function rmdirSync(path: string): void;
  function stat(path: string): Promise<Stats>;
  function statSync(path: string): Stats;
  function exists(path: string): Promise<boolean>;
  function existsSync(path: string): boolean;
  function access(path: string, mode?: number): Promise<void>;
  function accessSync(path: string, mode?: number): void;
  function chmod(path: string, mode: number | string): Promise<void>;
  function chmodSync(path: string, mode: number | string): void;
  function readdir(path: string): Promise<string[]>;
  function readdirSync(path: string): string[];
  function realpath(path: string): Promise<string>;
  function realpathSync(path: string): string;
  function readlink(path: string): Promise<string>;
  function readlinkSync(path: string): string;
  namespace realpathSync {
    function native(path: string): string;
  }
  function watch(path: string, listener?: (eventType: WatchEventType, filename?: string) => void): FSWatcher;
  function watch(
    path: string,
    options: WatchOptions | Encoding | 'buffer',
    listener?: (eventType: WatchEventType, filename?: string) => void
  ): FSWatcher;
  function watchFile(path: string, listener: (curr: Stats, prev: Stats) => void): void;
  function watchFile(path: string, options: WatchFileOptions, listener: (curr: Stats, prev: Stats) => void): void;
  function unwatchFile(path: string, listener?: (curr: Stats, prev: Stats) => void): void;
  const FSWatcher: {
    prototype: FSWatcher;
  };
  const ReadStream: {
    prototype: ReadStream;
    new (
      path: string,
      options?: {
        fd?: number;
        flags?: string | number;
        mode?: number;
        start?: number;
        end?: number;
        autoClose?: boolean;
        emitClose?: boolean;
        highWaterMark?: number;
      }
    ): ReadStream;
  };
  const WriteStream: {
    prototype: WriteStream;
    new (
      path: string,
      options?: {
        fd?: number;
        flags?: string | number;
        mode?: number;
        start?: number;
        autoClose?: boolean;
        emitClose?: boolean;
        highWaterMark?: number;
      }
    ): WriteStream;
  };
}

declare module 'ant:fs' {
  export * from 'fs';
}

declare module 'node:fs' {
  export * from 'fs';
}

declare module 'fs/promises' {
  interface FileHandle {
    readonly fd: number;
    close(): Promise<void>;
    stat(): Promise<Stats>;
    sync(): Promise<void>;
    read(
      buffer: ArrayBufferView,
      offset?: number,
      length?: number,
      position?: number | null
    ): Promise<{ bytesRead: number; buffer: ArrayBufferView }>;
    write(
      data: string | ArrayBufferView,
      offsetOrOptions?: number | { offset?: number; length?: number; position?: number | null },
      length?: number,
      position?: number | null
    ): Promise<{ bytesWritten: number; buffer: string | ArrayBufferView }>;
    writeFile(data: string | ArrayBufferView): Promise<void>;
  }

  interface Stats {
    size: number;
    mode: number;
    uid: number;
    gid: number;
    isFile(): boolean;
    isDirectory(): boolean;
    isSymbolicLink(): boolean;
  }

  type Encoding = 'utf8' | 'utf-8' | 'utf16le' | 'ucs2' | 'ucs-2' | 'latin1' | 'binary' | 'base64' | 'base64url' | 'hex' | 'ascii';

  const constants: {
    F_OK: number;
    R_OK: number;
    W_OK: number;
    X_OK: number;
    O_RDONLY: number;
    O_WRONLY: number;
    O_RDWR: number;
    O_CREAT: number;
    O_EXCL: number;
    O_TRUNC: number;
    O_APPEND: number;
  };

  function readFile(path: string, encoding: Encoding): Promise<string>;
  function readFile(path: string): Promise<Uint8Array>;
  function open(path: string, flags?: string, mode?: number): Promise<FileHandle>;
  function close(fd: number): Promise<void>;
  function writeFile(path: string, data: string | ArrayBufferView): Promise<void>;
  function write(fd: number, data: string | ArrayBufferView, offset?: number, length?: number, position?: number | null): Promise<number>;
  function writev(fd: number, buffers: ArrayBufferView[], position?: number): Promise<number>;
  function rm(path: string, options?: { recursive?: boolean; force?: boolean }): Promise<void>;
  function unlink(path: string): Promise<void>;
  function mkdir(path: string, options?: { recursive?: boolean; mode?: number }): Promise<void>;
  function rmdir(path: string): Promise<void>;
  function stat(path: string): Promise<Stats>;
  function exists(path: string): Promise<boolean>;
  function access(path: string, mode?: number): Promise<void>;
  function chmod(path: string, mode: number | string): Promise<void>;
  function readdir(path: string): Promise<string[]>;
  function realpath(path: string): Promise<string>;
  function readlink(path: string): Promise<string>;
}

declare module 'ant:fs/promises' {
  export * from 'fs/promises';
}

declare module 'node:fs/promises' {
  export * from 'fs/promises';
}
