export function normalizeVersion(version: string): string {
  return version.trim().replace(/^v/, '');
}

export function isOutOfDate(
  current: string,
  latest: string | undefined,
  latestSha: string | undefined,
): boolean | null {
  const normalizedCurrent = normalizeVersion(current);
  if (!normalizedCurrent) return null;
  if (latest) {
    const normalizedLatest = normalizeVersion(latest);
    if (normalizedCurrent === normalizedLatest) return false;
    if (/^\d+\.\d+\.\d+$/.test(normalizedLatest) && normalizedCurrent.startsWith(`${normalizedLatest}.`)) {
      return false;
    }
    return true;
  }

  const currentHash = normalizedCurrent.match(/-g([0-9a-fA-F]+)$/)?.[1].toLowerCase();
  if (!currentHash || !latestSha) return null;
  return !latestSha.toLowerCase().startsWith(currentHash);
}
