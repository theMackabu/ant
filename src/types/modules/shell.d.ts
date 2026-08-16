declare module 'ant:shell' {
  interface ShellOutput {
    stdout: Uint8Array;
    stderr: Uint8Array;
    exitCode: number;
    signalCode: string | null;
    text(): string;
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
    text(): Promise<string>;
    json<T = unknown>(): Promise<T>;
    arrayBuffer(): Promise<ArrayBuffer>;
    bytes(): Promise<Uint8Array>;
    blob(): Promise<Blob>;
    lines(): Promise<string[]>;
  }

  function $(strings: TemplateStringsArray, ...values: unknown[]): ShellPromise;
}
