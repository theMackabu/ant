const {
  ArrayIsArray: isArray,
  StringPrototypeSlice: slice,
  StringPrototypeIndexOf: indexOf,
  JSONStringify: stringify,
  String: SafeString,
  TypeError: SafeTypeError
} = require('ant:internal/primordials');

function receivedType(value) {
  if (value === null) return 'null';
  if (value === undefined) return 'undefined';

  const type = typeof value;
  switch (type) {
    case 'bigint':
      value = `${value}n`;
      break;
    case 'number':
      if (value === 0 && 1 / value === -Infinity) value = '-0';
      break;
    case 'symbol':
      value = SafeString(value);
      break;
    case 'function':
      return `function ${value.name}`;
    case 'object':
      if (value.constructor && 'name' in value.constructor) return `an instance of ${value.constructor.name}`;
      return require('node:util').inspect(value, { depth: -1 });
    case 'string':
      if (value.length > 28) value = `${slice(value, 0, 25)}...`;
      value = indexOf(value, "'") === -1 ? `'${value}'` : stringify(value);
      break;
  }

  return `type ${type} (${value})`;
}

function invalidArgType(name, expected, value) {
  const error = new SafeTypeError(`The "${name}" argument must be of type ${expected}. Received ${receivedType(value)}`);
  error.code = 'ERR_INVALID_ARG_TYPE';
  return error;
}

function validateString(value, name) {
  if (typeof value !== 'string') throw invalidArgType(name, 'string', value);
}

function validateObject(value, name) {
  if (value === null || isArray(value) || typeof value !== 'object') throw invalidArgType(name, 'object', value);
}

function getLazy(initializer) {
  let value;
  let initialized = false;
  return function () {
    if (!initialized) {
      value = initializer();
      initialized = true;
    }
    return value;
  };
}

module.exports = {
  CHAR_UPPERCASE_A: 65,
  CHAR_LOWERCASE_A: 97,
  CHAR_UPPERCASE_Z: 90,
  CHAR_LOWERCASE_Z: 122,
  CHAR_DOT: 46,
  CHAR_FORWARD_SLASH: 47,
  CHAR_BACKWARD_SLASH: 92,
  CHAR_COLON: 58,
  CHAR_QUESTION_MARK: 63,
  validateString,
  validateObject,
  getLazy,
  isWindows: process.platform === 'win32'
};
