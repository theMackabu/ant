type ProcessEnv = {
  [key: string]: string | undefined;
} & { toObject(): Record<string, string> };

interface Features {
  uv: boolean;
  tls_mbedtls: boolean;
  typescript: 'transform' | 'none';
}

interface Versions {
  node: string;
  ant: string;
  v8: string;
  uv: string;
  modules: string;
}

interface Process {
  env: ProcessEnv;
  argv: string[];
  pid: number;
  platform: string;
  arch: string;
  exit(code?: number): never;
  cwd(): string;
  dlopen(module: { exports?: unknown }, filename: string): void;
  features: Features;
  versions: Versions;
}

declare const process: Process;
