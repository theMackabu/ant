import { base64, numberParse, textFamily } from './codecs.mjs';
import { pathFamily, promise, property, regexp, streamShape } from './core.mjs';
import { jit } from './jit.mjs';
import { http1, websocket, websocketServer } from './network.mjs';
import { bufferBase64, json, number, string, structuredCloneFamily, textcodec, url } from './oracles.mjs';
import { escapes, jsonText, regexpGen, syntax } from './parsers.mjs';
import { jsExec } from './programs.mjs';

export const families = [
  property, regexp, promise, streamShape, pathFamily,
  json, number, string, structuredCloneFamily, textcodec, url, bufferBase64,
  base64, numberParse, textFamily, escapes, jsonText, regexpGen, syntax,
  http1, websocket, websocketServer,
  jit, jsExec
];

export const familyNames = Object.freeze(families.map(family => family.name));

export function familyNamed(name) {
  const family = families.find(candidate => candidate.name === name);
  if (!family) throw new Error(`unknown family: ${name}`);
  return family;
}
