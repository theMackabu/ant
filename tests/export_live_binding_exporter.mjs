export let counter = 0
export let compound = 0
export let destructured = 0
export { forwardLexical as forwardLexicalAlias }
let forwardLexical = 41
export { ForwardClass as ForwardClassAlias }
class ForwardClass {
  static value = 42
}
let inner = 0
export { inner as alias }
export let multi = 0
export { multi as multiB, multi as multiC }

export var varBinding = 0
export var deferredAlias
export { deferredAlias as deferredAliasA, deferredAlias as deferredAliasB }
export default function dflt() { return 'orig' }

export function bump() { counter++ }
export function writeAll() {
  compound += 5;
  [destructured] = [42];
  inner = 99;
  multi = 7;
  varBinding = 11;
}
export function swapDefault() { dflt = () => 'swapped' }
