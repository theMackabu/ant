declare module 'child_process' {
  interface SpawnResult {
    stdout: string;
    stderr: string;
    exitCode: number;
    signalCode: number | null;
    pid: number;
  }

  interface ChildProcess {
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

  interface SpawnSyncOptions {
    input?: string;
  }

  interface ForkOptions {
    execArgv?: string[];
  }

  function spawn(command: string, args?: string[], options?: SpawnOptions): ChildProcess & Promise<SpawnResult>;
  function exec(command: string, options?: ExecOptions): Promise<SpawnResult>;
  function execSync(command: string): string;
  function spawnSync(command: string, args?: string[], options?: SpawnSyncOptions): SpawnResult;
  function fork(modulePath: string, options?: ForkOptions): ChildProcess & Promise<SpawnResult>;
}
