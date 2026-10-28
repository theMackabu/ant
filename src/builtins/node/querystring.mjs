const defaultSepCodes = [38];
const defaultEqCodes = [61];

const isHexCode = code => (code >= 48 && code <= 57) || (code >= 65 && code <= 70) || (code >= 97 && code <= 102);
const hexValue = code => (code <= 57 ? code - 48 : (code | 32) - 87);

function createInvalidUriError() {
  const error = new URIError('URI malformed');
  error.code = 'ERR_INVALID_URI';
  return error;
}

export function escape(value) {
  let str = value;
  if (typeof str !== 'string') str = typeof str === 'object' ? String(str) : str + '';

  try {
    return encodeURIComponent(str);
  } catch {
    return encodeWithSurrogates(str);
  }
}

function encodeWithSurrogates(str) {
  let out = '';
  let last = 0;

  for (let i = 0; i < str.length; i++) {
    const code = str.charCodeAt(i);
    if (code < 0xd800 || code > 0xdfff) continue;
    if (i + 1 >= str.length) throw createInvalidUriError();

    const next = str.charCodeAt(i + 1) & 0x3ff;
    const point = 0x10000 + (((code & 0x3ff) << 10) | next);
    out += encodeURIComponent(str.slice(last, i)) + encodeURIComponent(String.fromCodePoint(point));
    last = i + 2;
    i++;
  }

  return out + encodeURIComponent(str.slice(last));
}

export function unescapeBuffer(value, decodeSpaces) {
  const s = String(value);
  const out = Buffer.allocUnsafe(s.length);
  const maxLength = s.length - 2;
  
  let index = 0;
  let outIndex = 0;
  let hasHex = false;

  while (index < s.length) {
    let code = s.charCodeAt(index);
    if (code === 43 && decodeSpaces) {
      out[outIndex++] = 32;
      index++;
      continue;
    }

    if (code === 37 && index < maxLength) {
      code = s.charCodeAt(++index);
      if (!isHexCode(code)) {
        out[outIndex++] = 37;
        continue;
      }
      const high = hexValue(code);
      const next = s.charCodeAt(++index);
      if (!isHexCode(next)) {
        out[outIndex++] = 37;
        index--;
      } else {
        hasHex = true;
        code = high * 16 + hexValue(next);
      }
    }

    out[outIndex++] = code;
    index++;
  }

  return hasHex ? out.subarray(0, outIndex) : out;
}

export function unescape(value, decodeSpaces) {
  try {
    return decodeURIComponent(value);
  } catch {
    return querystring.unescapeBuffer(value, decodeSpaces).toString();
  }
}

const charCodes = str => {
  const codes = [];
  for (let i = 0; i < str.length; i++) codes.push(str.charCodeAt(i));
  return codes;
};

function decodeStr(value, decoder) {
  try {
    return decoder(value);
  } catch {
    return querystring.unescape(value, true);
  }
}

function addKeyVal(obj, key, value, keyEncoded, valEncoded, decoder) {
  if (key.length > 0 && keyEncoded) key = decodeStr(key, decoder);
  if (value.length > 0 && valEncoded) value = decodeStr(value, decoder);

  const current = obj[key];
  if (current === undefined) obj[key] = value;
  else if (current.pop) current[current.length] = value;
  else obj[key] = [current, value];
}

export function parse(qs, sep, eq, options) {
  const obj = { __proto__: null };
  if (typeof qs !== 'string' || qs.length === 0) return obj;

  const sepCodes = sep ? charCodes(String(sep)) : defaultSepCodes;
  const eqCodes = eq ? charCodes(String(eq)) : defaultEqCodes;
  const sepLen = sepCodes.length;
  const eqLen = eqCodes.length;

  let pairs = 1000;
  if (options && typeof options.maxKeys === 'number') pairs = options.maxKeys > 0 ? options.maxKeys : -1;

  let decoder = querystring.unescape;
  if (options && typeof options.decodeURIComponent === 'function') decoder = options.decodeURIComponent;
  const customDecode = decoder !== unescape;

  let lastPos = 0;
  let sepIdx = 0;
  let eqIdx = 0;
  let key = '';
  let value = '';
  let keyEncoded = customDecode;
  let valEncoded = customDecode;
  const plusChar = customDecode ? '%20' : ' ';
  let encodeCheck = 0;

  for (let i = 0; i < qs.length; ++i) {
    const code = qs.charCodeAt(i);

    if (code === sepCodes[sepIdx]) {
      if (++sepIdx === sepLen) {
        const end = i - sepIdx + 1;
        if (eqIdx < eqLen) {
          if (lastPos < end) key += qs.slice(lastPos, end);
          else if (key.length === 0) {
            if (--pairs === 0) return obj;
            lastPos = i + 1;
            sepIdx = eqIdx = 0;
            continue;
          }
        } else if (lastPos < end) value += qs.slice(lastPos, end);

        addKeyVal(obj, key, value, keyEncoded, valEncoded, decoder);
        if (--pairs === 0) return obj;

        keyEncoded = valEncoded = customDecode;
        key = value = '';
        encodeCheck = 0;
        lastPos = i + 1;
        sepIdx = eqIdx = 0;
      }
      continue;
    }

    sepIdx = 0;
    if (eqIdx < eqLen) {
      if (code === eqCodes[eqIdx]) {
        if (++eqIdx === eqLen) {
          const end = i - eqIdx + 1;
          if (lastPos < end) key += qs.slice(lastPos, end);
          encodeCheck = 0;
          lastPos = i + 1;
        }
        continue;
      }

      eqIdx = 0;
      if (!keyEncoded) {
        if (code === 37) {
          encodeCheck = 1;
          continue;
        }
        if (encodeCheck > 0) {
          if (isHexCode(code)) {
            if (++encodeCheck === 3) keyEncoded = true;
            continue;
          }
          encodeCheck = 0;
        }
      }

      if (code === 43) {
        if (lastPos < i) key += qs.slice(lastPos, i);
        key += plusChar;
        lastPos = i + 1;
        continue;
      }
    }

    if (code === 43) {
      if (lastPos < i) value += qs.slice(lastPos, i);
      value += plusChar;
      lastPos = i + 1;
    } else if (!valEncoded) {
      if (code === 37) encodeCheck = 1;
      else if (encodeCheck > 0) {
        if (isHexCode(code)) {
          if (++encodeCheck === 3) valEncoded = true;
        } else encodeCheck = 0;
      }
    }
  }

  if (lastPos < qs.length) {
    if (eqIdx < eqLen) key += qs.slice(lastPos);
    else if (sepIdx < sepLen) value += qs.slice(lastPos);
  } else if (eqIdx === 0 && key.length === 0) return obj;

  addKeyVal(obj, key, value, keyEncoded, valEncoded, decoder);
  return obj;
}

function stringifyPrimitive(value) {
  if (typeof value === 'string') return value;
  if (typeof value === 'number' && isFinite(value)) return '' + value;
  if (typeof value === 'bigint') return '' + value;
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  return '';
}

function encodeStringified(value, encoder) {
  if (typeof value === 'string') return value.length ? encoder(value) : '';
  if (typeof value === 'number' && isFinite(value)) return Math.abs(value) < 1e21 ? '' + value : encoder('' + value);
  if (typeof value === 'bigint') return '' + value;
  if (typeof value === 'boolean') return value ? 'true' : 'false';
  return '';
}

const encodeStringifiedCustom = (value, encoder) => encoder(stringifyPrimitive(value));

export function stringify(obj, sep, eq, options) {
  sep ||= '&';
  eq ||= '=';

  let encoder = querystring.escape;
  if (options && typeof options.encodeURIComponent === 'function') encoder = options.encodeURIComponent;
  const convert = encoder === escape ? encodeStringified : encodeStringifiedCustom;

  if (obj === null || typeof obj !== 'object') return '';

  let fields = '';
  for (const key of Object.keys(obj)) {
    const value = obj[key];
    const prefix = convert(key, encoder) + eq;

    if (Array.isArray(value)) {
      if (value.length === 0) continue;
      if (fields) fields += sep;
      for (let j = 0; j < value.length; ++j) {
        if (j) fields += sep;
        fields += prefix + convert(value[j], encoder);
      }
      continue;
    }

    if (fields) fields += sep;
    fields += prefix + convert(value, encoder);
  }

  return fields;
}

export const encode = stringify;
export const decode = parse;

const querystring = { unescapeBuffer, unescape, escape, stringify, encode, parse, decode };
export default querystring;
