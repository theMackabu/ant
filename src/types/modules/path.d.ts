declare module 'path' {
  interface ParsedPath {
    root: string;
    dir: string;
    base: string;
    ext: string;
    name: string;
  }

  interface PathModule {
    sep: string;
    delimiter: string;
    basename(path: string, ext?: string): string;
    dirname(path: string): string;
    extname(path: string): string;
    join(...paths: string[]): string;
    normalize(path: string): string;
    resolve(...paths: string[]): string;
    relative(from: string, to: string): string;
    isAbsolute(path: string): boolean;
    parse(path: string): ParsedPath;
    format(pathObject: Partial<ParsedPath>): string;
    posix: PathModule;
    win32: PathModule;
  }

  const path: PathModule;
  export = path;
}
