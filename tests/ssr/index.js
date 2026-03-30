export async function loadRscIndexLater() {
  const mod = await import('../rsc/index.js');
  return mod.marker;
}
