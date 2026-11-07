import { RESPONSE_ERROR, RESPONSE_OK } from './abi.js';
import { transferableError } from './errors.js';
import { checkedRange, copyIn, writeU32 } from './memory.js';
import { decodeWire, encodeWire } from './wire.js';

function callHost(memory, hostFunctions, functionId, argumentsPointer, argumentsLength) {
  try {
    argumentsPointer = checkedRange(memory, argumentsPointer, argumentsLength);
    const fn = hostFunctions[functionId];
    if (typeof fn !== 'function') throw new TypeError('Unknown host function');
    const value = fn(...decodeWire(new Uint8Array(memory.buffer, argumentsPointer, argumentsLength)));
    if (value !== null && (typeof value === 'object' || typeof value === 'function') && typeof value.then === 'function') {
      throw new TypeError('Host functions must return synchronously');
    }
    return encodeWire(value, RESPONSE_OK);
  } catch (error) {
    try {
      return encodeWire(transferableError(error), RESPONSE_ERROR);
    } catch {
      return encodeWire({ name: 'Error', message: 'Host function failed' }, RESPONSE_ERROR);
    }
  }
}

export function antImports(memory, hostFunctions, getExports) {
  return {
    host_call(functionId, argumentsPointer, argumentsLength, responseLengthPointer) {
      const response = callHost(memory, hostFunctions, functionId, argumentsPointer, argumentsLength);
      const pointer = copyIn(getExports(), memory, response);
      writeU32(memory, responseLengthPointer, pointer ? response.length : 0);
      return pointer;
    },
    now_ms: () => globalThis.performance?.now?.() ?? Date.now()
  };
}
