declare module 'fs' {
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
  const promises: typeof import('fs/promises');

  function readFile(path: string, encoding: Encoding): Promise<string>;
  function readFile(path: string): Promise<Uint8Array>;
  function readFileSync(path: string, encoding: Encoding | { encoding: Encoding }): string;
  function readFileSync(path: string): Uint8Array;
  function readSync(fd: number, buffer: ArrayBufferView, offset?: number, length?: number, position?: number | null): number;
  function stream(path: string): Promise<string>;
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
  function unlink(path: string): Promise<void>;
  function unlinkSync(path: string): void;
  function mkdir(path: string, options?: { recursive?: boolean; mode?: number }): Promise<void>;
  function mkdirSync(path: string, options?: number | { recursive?: boolean; mode?: number }): void;
  function rmdir(path: string): Promise<void>;
  function rmdirSync(path: string): void;
  function stat(path: string): Promise<Stats>;
  function statSync(path: string): Stats;
  function exists(path: string): Promise<boolean>;
  function existsSync(path: string): boolean;
  function access(path: string, mode?: number): Promise<void>;
  function accessSync(path: string, mode?: number): void;
  function readdir(path: string): Promise<string[]>;
  function readdirSync(path: string): string[];
}

declare module 'ant:fs' {
  export * from 'fs';
}

declare module 'node:fs' {
  export * from 'fs';
}

declare module 'fs/promises' {
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
  function open(path: string, flags?: string, mode?: number): Promise<number>;
  function close(fd: number): Promise<void>;
  function writeFile(path: string, data: string | ArrayBufferView): Promise<void>;
  function write(fd: number, data: string | ArrayBufferView, offset?: number, length?: number, position?: number | null): Promise<number>;
  function writev(fd: number, buffers: ArrayBufferView[], position?: number): Promise<number>;
  function unlink(path: string): Promise<void>;
  function mkdir(path: string, options?: { recursive?: boolean; mode?: number }): Promise<void>;
  function rmdir(path: string): Promise<void>;
  function stat(path: string): Promise<Stats>;
  function exists(path: string): Promise<boolean>;
  function access(path: string, mode?: number): Promise<void>;
  function readdir(path: string): Promise<string[]>;
}

declare module 'ant:fs/promises' {
  export * from 'fs/promises';
}

declare module 'node:fs/promises' {
  export * from 'fs/promises';
}
