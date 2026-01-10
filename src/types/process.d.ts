type ProcessEnv = {
  [key: string]: string | undefined;
} & { toObject(): Record<string, string> };

interface Process {
  env: ProcessEnv;
  argv: string[];
  pid: number;
  platform: string;
  arch: string;
  exit(code?: number): never;
  cwd(): string;
}

declare const process: Process;
