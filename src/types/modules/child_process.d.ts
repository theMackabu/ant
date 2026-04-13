declare module 'child_process' {
  import { EventEmitter } from 'events';

  interface SpawnResult {
    stdout: string;
    stderr: string;
    exitCode: number;
    signalCode: number | null;
    pid: number;
  }

  class ChildProcess extends EventEmitter {
    stdout: string;
    stderr: string;
    exitCode: number | null;
    signalCode: number | null;
    pid: number;
    killed: boolean;
    on(event: 'exit', listener: (code: number, signal: number | null) => void): ChildProcess;
    on(event: 'close', listener: (code: number, signal: number | null) => void): ChildProcess;
    on(event: 'error', listener: (err: Error) => void): ChildProcess;
    on(event: 'data', listener: (data: string) => void): ChildProcess;
    once(event: string, listener: (...args: unknown[]) => void): ChildProcess;
    kill(signal?: number): boolean;
    write(data: string): void;
    end(): void;
  }

  interface SpawnOptions {
    cwd?: string;
    shell?: boolean;
    detached?: boolean;
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
  }

  interface ForkOptions {
    execArgv?: string[];
  }

  function spawn(command: string, args?: string[], options?: SpawnOptions): ChildProcess & Promise<SpawnResult>;
  function exec(command: string, options?: ExecOptions): Promise<SpawnResult>;
  function execFile(file: string, args?: string[], options?: ExecOptions): ChildProcess;
  function execFile(
    file: string,
    args: string[] | undefined,
    options: ExecOptions | undefined,
    callback: (err: Error | null, stdout: string, stderr: string) => void
  ): ChildProcess;
  function execSync(command: string): string;
  function spawnSync(command: string, args?: string[], options?: SpawnSyncOptions): SpawnResult;
  function fork(modulePath: string, options?: ForkOptions): ChildProcess & Promise<SpawnResult>;
}

declare module 'ant:child_process' {
  export * from 'events';
}

declare module 'node:child_process' {
  export * from 'events';
}
