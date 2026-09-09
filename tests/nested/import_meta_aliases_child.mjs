export const meta = import.meta;

export function readMeta() {
  return import.meta;
}

export async function readAsyncMeta() {
  await 0;
  return import.meta;
}
