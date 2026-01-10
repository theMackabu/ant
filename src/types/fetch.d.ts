interface FetchResponse {
  status: number;
  ok: boolean;
  body: string;
  json(): unknown;
}

interface FetchOptions {
  method?: string;
  headers?: Record<string, string>;
  body?: string;
}

declare function fetch(url: string, options?: FetchOptions): Promise<FetchResponse>;
