declare module 'ant:shell' {
  interface ShellResult {
    stdout: string;
    stderr: string;
    exitCode: number;
    text(): string;
    lines(): string[];
  }

  function $(strings: TemplateStringsArray, ...values: unknown[]): ShellResult;
  function $(command: string): ShellResult;
}
