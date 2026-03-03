export enum LispNode {
  APPLY,
  WORD,
  ATOM,
  STRING
}

export type Env = Record<string, LispValue>;
export type Leaf = [LispNode, string | number];
export type Expr = Leaf | Expr[];

export type LispValue = number | string | LispValue[] | LispFn;
export type LispFn = (args: Expr[], env: Env) => LispValue;

export const Leaf = {
  apply: (value: string): Leaf => [LispNode.APPLY, value],
  word: (value: string): Leaf => [LispNode.WORD, value],
  atom: (value: number): Leaf => [LispNode.ATOM, value],
  string: (value: string): Leaf => [LispNode.STRING, value]
};
