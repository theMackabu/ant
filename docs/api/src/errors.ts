export class HttpError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message);
  }
}

export function isNotFound(error: unknown): boolean {
  return error instanceof HttpError && error.status === 404;
}
