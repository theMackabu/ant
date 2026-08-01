declare module 'child_process' {
  import { EventEmitter } from 'events';
  import { Readable, Writable } from 'stream';

  interface SpawnResult {
    stdout: string;
    stderr: string;
    exitCode: number;
    signalCode: string | null;
    pid: number;
  }

  class ChildProcess extends EventEmitter {
    stdout: Readable;
    stderr: Readable;
    stdin: Writable;
    exitCode: number | null;
    signalCode: string | null;
    pid: number;
    killed: boolean;
    on(event: 'exit', listener: (code: number | null, signal: string | null) => void): ChildProcess;
    on(event: 'close', listener: (code: number | null, signal: string | null) => void): ChildProcess;
    on(event: 'error', listener: (err: Error) => void): ChildProcess;
    once(event: string, listener: (...args: unknown[]) => void): ChildProcess;
    kill(signal?: number | string): boolean;
    write(data: string): void;
    end(): void;
  }

  interface SpawnOptions {
    cwd?: string;
    shell?: boolean;
    detached?: boolean;
    env?: Record<string, string>;
    stdio?: 'pipe' | 'inherit' | 'ignore' | Array<'pipe' | 'inherit' | 'ignore'>;
  }

  interface ExecOptions {
    cwd?: string;
  }

  interface ExecFileResult {
    stdout: string;
    stderr: string;
  }

  interface SpawnSyncOptions {
    input?: string;
    cwd?: string;
    shell?: boolean;
    env?: Record<string, string>;
    stdio?: 'pipe' | 'inherit' | 'ignore' | Array<'pipe' | 'inherit' | 'ignore'>;
    timeout?: number;
    killSignal?: string | number;
  }

  interface SpawnSyncResult {
    stdout: string;
    stderr: string;
    status: number | null;
    signal: string | null;
    pid: number;
    error?: Error;
  }

  type ExecSyncOptions = Omit<SpawnSyncOptions, 'shell'>;

  interface ExecSyncError extends Error {
    status: number | null;
    signal: string | null;
    stdout: string;
    stderr: string;
    pid: number;
    cmd: string;
  }

  interface ForkOptions {
    execArgv?: string[];
  }

  function spawn(command: string, args?: string[], options?: SpawnOptions): ChildProcess & Promise<SpawnResult>;
  function spawn(command: string, options?: SpawnOptions): ChildProcess & Promise<SpawnResult>;
  function exec(command: string, options?: ExecOptions): Promise<SpawnResult>;
  function execFile(file: string, args?: string[], options?: ExecOptions): ChildProcess;
  function execFile(
    file: string,
    args: string[] | undefined,
    options: ExecOptions | undefined,
    callback: (err: Error | null, stdout: string, stderr: string) => void
  ): ChildProcess;
  function execSync(command: string, options?: ExecSyncOptions): string;
  function execFileSync(file: string, args?: string[], options?: SpawnSyncOptions): string;
  function execFileSync(file: string, options?: SpawnSyncOptions): string;
  function spawnSync(command: string, args?: string[], options?: SpawnSyncOptions): SpawnSyncResult;
  function spawnSync(command: string, options?: SpawnSyncOptions): SpawnSyncResult;
  function fork(modulePath: string, options?: ForkOptions): ChildProcess & Promise<SpawnResult>;
}

declare module 'ant:child_process' {
  export * from 'events';
}

declare module 'node:child_process' {
  export * from 'events';
}
