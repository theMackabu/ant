interface ImportMeta {
  url: string;
  filename: string;
  dirname: string;
  dir: string;
  path: string;
  file: string;
  main: boolean;
  resolve(specifier: string): string;

  env: ProcessEnv;
}
