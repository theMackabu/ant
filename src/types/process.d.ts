type ProcessEnv = {
  [key: string]: string | undefined;
} & { toObject(): Record<string, string> };

interface Features {
  uv: boolean;
  tls_mbedtls: boolean;
  typescript: 'transform' | 'none';
}

interface Process {
  env: ProcessEnv;
  argv: string[];
  pid: number;
  platform: string;
  arch: string;
  exit(code?: number): never;
  cwd(): string;
  features: Features;
}

declare const process: Process;
