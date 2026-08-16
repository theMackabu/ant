declare module 'ant:shell' {
  interface ShellOutput {
    stdout: string;
    stderr: string;
    exitCode: number;
    signalCode: string | null;
  }

  interface ShellPromise extends PromiseLike<ShellOutput> {
    catch<TResult = never>(
      onRejected?: ((reason: unknown) => TResult | PromiseLike<TResult>) | null
    ): Promise<ShellOutput | TResult>;
    nothrow(): ShellPromise;
    text(): Promise<string>;
    lines(): Promise<string[]>;
  }

  function $(strings: TemplateStringsArray, ...values: unknown[]): ShellPromise;
}
