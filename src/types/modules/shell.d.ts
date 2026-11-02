declare module 'ant:shell' {
  interface ShellOutputMethods {
    text(encoding?: string): string;
    json<T = unknown>(): T;
    arrayBuffer(): ArrayBuffer;
    bytes(): Uint8Array;
    blob(): Blob;
    lines(): string[];
  }

  interface ShellOutput extends ShellOutputMethods {
    stdout: Uint8Array & BufferMethods;
    stderr: Uint8Array & BufferMethods;
    exitCode: number;
    signalCode: string | null;
  }

  class ShellError extends Error implements ShellOutputMethods {
    stdout: Uint8Array & BufferMethods;
    stderr: Uint8Array & BufferMethods;
    exitCode: number;
    signalCode: string | null;
    text(encoding?: string): string;
    json<T = unknown>(): T;
    arrayBuffer(): ArrayBuffer;
    bytes(): Uint8Array;
    blob(): Blob;
    lines(): string[];
  }

  interface ShellPromise extends PromiseLike<ShellOutput> {
    catch<TResult = never>(
      onRejected?: ((reason: unknown) => TResult | PromiseLike<TResult>) | null
    ): Promise<ShellOutput | TResult>;
    finally(onFinally?: (() => void) | null): Promise<ShellOutput>;
    nothrow(): ShellPromise;
    throws(shouldThrow?: boolean): ShellPromise;
    quiet(quiet?: boolean): ShellPromise;
    env(values?: Record<string, string | undefined>): ShellPromise;
    cwd(directory?: string): ShellPromise;
    text(encoding?: string): Promise<string>;
    json<T = unknown>(): Promise<T>;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
    blob(): Promise<Blob>;
    lines(): Promise<string[]> & AsyncIterable<string>;
  }

  interface Shell {
    (strings: TemplateStringsArray, ...values: unknown[]): ShellPromise;
    env(values?: Record<string, string | undefined>): Shell;
    cwd(directory?: string): Shell;
    nothrow(): Shell;
    throws(shouldThrow?: boolean): Shell;
    quiet(quiet?: boolean): Shell;
    escape(value: string): string;
    braces(pattern: string): string[];
    Shell: new () => Shell;
    ShellError: typeof ShellError;
  }

  const $: Shell;
}
