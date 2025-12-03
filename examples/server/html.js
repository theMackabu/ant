export function html(strings, ...values) {
  let result = '';
  for (let i = 0; i < strings.length; i++) {
    result = result + strings[i];
    if (i < values.length) {
      let escaped = values[i];
      result = result + escaped;
    }
  }
  return result;
}
