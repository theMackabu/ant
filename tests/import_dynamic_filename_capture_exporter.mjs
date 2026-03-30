export async function loadChildLater() {
  const child = await import('./import_dynamic_filename_capture_child.mjs');
  return child.childFilename;
}
