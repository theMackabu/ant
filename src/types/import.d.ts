interface ImportMeta {
  url: string;
  filename: boolean;
  dirname: string;
  main: boolean;
  resolve(specifier: string): string;

  readonly env: { [key: string]: string };
}
