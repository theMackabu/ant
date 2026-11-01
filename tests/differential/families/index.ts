import { pathFamily, promise, property, regexp, streamShape } from './core.ts';
import type { AnyFamily } from './family.ts';
import { jit } from './jit.ts';
import { bufferBase64, json, number, string, structuredCloneFamily, textcodec, url } from './oracles.ts';

export const families: readonly AnyFamily[] = [
  property, regexp, promise, streamShape, pathFamily,
  json, number, string, structuredCloneFamily, textcodec, url, bufferBase64,
  jit
];

export const familyNames = Object.freeze(families.map(family => family.name));

export function familyNamed(name: string): AnyFamily {
  const family = families.find(candidate => candidate.name === name);
  if (!family) throw new Error(`unknown family: ${name}`);
  return family;
}
