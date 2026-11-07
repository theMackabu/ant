export function writeUtf16(view, offset, value) {
  for (let index = 0; index < value.length; index++) view.setUint16(offset + index * 2, value.charCodeAt(index), true);
}

export function readUtf16(view, offset, count) {
  const units = new Uint16Array(Math.min(count, 8192));
  let result = '';
  for (let start = 0; start < count; start += units.length) {
    const size = Math.min(units.length, count - start);
    for (let index = 0; index < size; index++) units[index] = view.getUint16(offset + (start + index) * 2, true);
    result += String.fromCharCode(...units.subarray(0, size));
  }
  return result;
}
