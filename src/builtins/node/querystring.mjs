const plusPattern = /\+/g;
const defaultMaxKeys = 1000;

export const escape = value => encodeURIComponent(String(value));
export const normalizeComponent = value => String(value).replace(plusPattern, '%20');

export function unescape(value) {
  const input = normalizeComponent(value);

  try {
    return decodeURIComponent(input);
  } catch {
    return input;
  }
}

const getOptions = options => (options && typeof options === 'object' ? options : null);

const appendValue = (target, key, value) => {
  if (target[key] === undefined) {
    target[key] = value;
    return;
  }

  target[key] = Array.isArray(target[key]) ? [...target[key], value] : [target[key], value];
};

export function parse(input, sep = '&', eq = '=', options = undefined) {
  const result = Object.create(null);
  const source = String(input || '');
  const parsedOptions = getOptions(options);
  const maxKeys = typeof parsedOptions?.maxKeys === 'number' ? parsedOptions.maxKeys : defaultMaxKeys;
  const decode = typeof parsedOptions?.decodeURIComponent === 'function' ? parsedOptions.decodeURIComponent : unescape;

  if (source === '') return result;

  let count = 0;
  for (const entry of source.split(sep)) {
    if (entry === '') continue;
    if (maxKeys > 0 && count >= maxKeys) break;

    const index = entry.indexOf(eq);
    const rawKey = index >= 0 ? entry.slice(0, index) : entry;
    const rawValue = index >= 0 ? entry.slice(index + eq.length) : '';
    const key = decode(normalizeComponent(rawKey));
    const value = decode(normalizeComponent(rawValue));

    appendValue(result, key, value);
    count += 1;
  }

  return result;
}

export function stringify(obj, sep = '&', eq = '=', options = undefined) {
  const parsedOptions = getOptions(options);
  const encode = typeof parsedOptions?.encodeURIComponent === 'function' ? parsedOptions.encodeURIComponent : escape;

  if (obj === null || obj === undefined) return '';
  const parts = [];

  for (const key of Object.keys(obj)) {
    const value = obj[key];
    const encodedKey = encode(key);

    if (Array.isArray(value)) {
      for (const item of value) parts.push(encodedKey + eq + encode(item ?? ''));
      continue;
    }

    parts.push(encodedKey + eq + encode(value ?? ''));
  }

  return parts.join(sep);
}

export const encode = stringify;
export const decode = parse;

export default { parse, stringify, escape, unescape, encode, decode };
