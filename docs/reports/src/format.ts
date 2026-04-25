export function formatBytes(value: number | null): string {
  if (!value || !Number.isFinite(value)) return 'unknown';
  if (value >= 1024 * 1024) return `${Math.round(value / 1024 / 1024)}mb`;
  if (value >= 1024) return `${Math.round(value / 1024)}kb`;
  return `${value}b`;
}

export function crashDetail(code: string): string {
  switch (code) {
    case 'SIGSEGV':
    case 'EXCEPTION_ACCESS_VIOLATION':
      return 'Invalid memory access';
    case 'SIGBUS':
      return 'Bus error';
    case 'SIGFPE':
      return 'Floating point exception';
    case 'SIGILL':
    case 'EXCEPTION_ILLEGAL_INSTRUCTION':
      return 'Illegal instruction';
    case 'SIGABRT':
      return 'Abort';
    case 'EXCEPTION_STACK_OVERFLOW':
      return 'Stack overflow';
    default:
      return 'Fatal error';
  }
}
