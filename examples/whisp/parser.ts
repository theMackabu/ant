import * as Types from './types';

type TokenHandler = (ctx: ParseContext) => void;

interface ParseContext {
  source: string;
  i: number;
  head: Types.Expr[];
  stack: Types.Expr[][];
  acc: string;
}

const openParen = (ctx: ParseContext): void => {
  if (!ctx.head) {
    const around = ctx.source.substring(Math.max(0, ctx.i - 20), ctx.i + 20);
    throw new Error(`Unexpected '(' at position ${ctx.i} (stack empty): ...${around}...`);
  }
  const temp: Types.Expr[] = [];
  ctx.head.push(temp);
  ctx.stack.push(ctx.head);
  ctx.head = temp;
};

const closeParen = (ctx: ParseContext): void => {
  flushToken(ctx);
  if (ctx.stack.length === 0) {
    const around = ctx.source.substring(Math.max(0, ctx.i - 20), ctx.i + 20);
    throw new Error(`Unmatched ')' at position ${ctx.i}: ...${around}...`);
  }
  ctx.head = ctx.stack.pop()!;
};

const flushToken = (ctx: ParseContext): void => {
  const token = ctx.acc;
  ctx.acc = '';
  if (!token) return;

  if (!ctx.head.length) ctx.head.push(Types.Leaf.apply(token));
  else if (/^-?\d+(\.\d+)?$/.test(token)) ctx.head.push(Types.Leaf.atom(Number(token)));
  else ctx.head.push(Types.Leaf.word(token));
};

const parseString = (ctx: ParseContext): void => {
  let str = '';
  const escapes: Record<string, string> = { n: '\n', t: '\t', '\\': '\\', '"': '"' };

  while (++ctx.i < ctx.source.length && ctx.source[ctx.i] !== '"') {
    if (ctx.source[ctx.i] === '\\' && ctx.i + 1 < ctx.source.length) {
      const next = ctx.source[++ctx.i];
      str += escapes[next] ?? '\\' + next;
    } else str += ctx.source[ctx.i];
  }
  ctx.head.push(Types.Leaf.string(str));
};

const skipComment = (ctx: ParseContext): void => {
  flushToken(ctx);
  while (ctx.i < ctx.source.length && ctx.source[ctx.i] !== '\n') ctx.i++;
};

const tokenTable: Record<string, TokenHandler> = {
  '"': parseString,
  '(': openParen,
  ')': closeParen,
  ';': skipComment,
  ' ': flushToken,
  '\n': flushToken,
  '\r': flushToken,
  '\t': flushToken
};

export function parse(source: string): Types.Expr {
  const tree: Types.Expr[] = [];

  const ctx: ParseContext = {
    source,
    i: 0,
    head: tree,
    stack: [tree],
    acc: ''
  };

  for (; ctx.i < source.length; ctx.i++) {
    const handler = tokenTable[source[ctx.i]];
    if (handler) handler(ctx);
    else ctx.acc += source[ctx.i];
  }

  return tree[0];
}
