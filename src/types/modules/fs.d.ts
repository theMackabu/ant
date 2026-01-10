declare module 'fs' {
  interface Stats {
    size: number;
    mode: number;
    isFile(): boolean;
    isDirectory(): boolean;
  }

  function readFile(path: string): Promise<string>;
  function readFileSync(path: string): string;
  function stream(path: string): Promise<string>;
  function open(path: string): string;
  function writeFile(path: string, data: string): Promise<void>;
  function writeFileSync(path: string, data: string): void;
  function appendFileSync(path: string, data: string): void;
  function copyFileSync(src: string, dest: string): void;
  function renameSync(oldPath: string, newPath: string): void;
  function unlink(path: string): Promise<void>;
  function unlinkSync(path: string): void;
  function mkdir(path: string, options?: { recursive?: boolean }): Promise<void>;
  function mkdirSync(path: string, options?: { recursive?: boolean }): void;
  function rmdir(path: string): Promise<void>;
  function rmdirSync(path: string): void;
  function stat(path: string): Promise<Stats>;
  function statSync(path: string): Stats;
  function exists(path: string): Promise<boolean>;
  function existsSync(path: string): boolean;
  function readdir(path: string): Promise<string[]>;
  function readdirSync(path: string): string[];
}
