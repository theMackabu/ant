import { pathFamily, promise, property, regexp, streamShape } from './core.mjs';
import { jit } from './jit.mjs';
import { bufferBase64, json, number, string, structuredCloneFamily, textcodec, url } from './oracles.mjs';

export const families = [
  property, regexp, promise, streamShape, pathFamily,
  json, number, string, structuredCloneFamily, textcodec, url, bufferBase64,
  jit
];

export const familyNames = Object.freeze(families.map(family => family.name));

export function familyNamed(name) {
  const family = families.find(candidate => candidate.name === name);
  if (!family) throw new Error(`unknown family: ${name}`);
  return family;
}
