import { parse } from './parser';
import * as Types from './types';

const TRUE = 1;
const FALSE = 0;

const handlers: Record<Types.LispNode, (value: any, args: Types.Expr[], env: Types.Env) => Types.LispValue> = {
  [Types.LispNode.WORD]: (value, _, env) => env[value],
  [Types.LispNode.APPLY]: (value, args, env) => (env[value] as Types.LispFn)(args, env),
  [Types.LispNode.ATOM]: value => value,
  [Types.LispNode.STRING]: value => value
};

const Eval = {
  name: (exp: Types.Expr) => { const [, value] = exp as Types.Leaf; return value; },
  num: (exp: Types.Expr, env: Types.Env): number => evaluate(exp, env) as number,
  str: (exp: Types.Expr, env: Types.Env): string => evaluate(exp, env) as string,
  arr: (exp: Types.Expr, env: Types.Env): Types.LispValue[] => evaluate(exp, env) as Types.LispValue[],
  fn: (exp: Types.Expr, env: Types.Env): Types.LispFn => evaluate(exp, env) as Types.LispFn
};

function isLeaf(exp: Types.Expr): exp is Types.Leaf {
  if (!Array.isArray(exp)) return false;
  return typeof exp[0] === 'number' && exp[0] >= Types.LispNode.APPLY && exp[0] <= Types.LispNode.STRING;
}

function l_mod(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) % Eval.num(args[1], env);
}

function l_div(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) / Eval.num(args[1], env);
}

function l_length(args: Types.Expr[], env: Types.Env): number {
  return (evaluate(args[0], env) as string | Types.LispValue[]).length;
}

function l_add(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) + Eval.num(args[1], env);
}

function l_mul(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) * Eval.num(args[1], env);
}

function l_sub(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) - Eval.num(args[1], env);
}

function l_array(args: Types.Expr[], env: Types.Env): Types.LispValue[] {
  return args.length ? args.map(x => evaluate(x, env)) : [];
}

function l_get(args: Types.Expr[], env: Types.Env): Types.LispValue {
  return Eval.arr(args[0], env)[Eval.num(args[1], env)];
}

function l_do(args: Types.Expr[], env: Types.Env): Types.LispValue {
  return args.reduce<Types.LispValue>((_, x) => evaluate(x, env), FALSE);
}

function l_not(args: Types.Expr[], env: Types.Env): number {
  return +!evaluate(args[0], env);
}

function l_eq(args: Types.Expr[], env: Types.Env): number {
  return +(evaluate(args[0], env) === evaluate(args[1], env));
}

function l_lt(args: Types.Expr[], env: Types.Env): number {
  return +(evaluate(args[0], env) < evaluate(args[1], env));
}

function l_gt(args: Types.Expr[], env: Types.Env): number {
  return +(evaluate(args[0], env) > evaluate(args[1], env));
}

function l_gte(args: Types.Expr[], env: Types.Env): number {
  return +(evaluate(args[0], env) >= evaluate(args[1], env));
}

function l_lte(args: Types.Expr[], env: Types.Env): number {
  return +(evaluate(args[0], env) <= evaluate(args[1], env));
}

function l_and(args: Types.Expr[], env: Types.Env): Types.LispValue {
  return !evaluate(args[0], env) ? FALSE : evaluate(args[1], env);
}

function l_or(args: Types.Expr[], env: Types.Env): Types.LispValue {
  return evaluate(args[0], env) ? TRUE : evaluate(args[1], env);
}

function l_apply(args: Types.Expr[], env: Types.Env): Types.LispValue {
  return Eval.fn(args.pop()!, env)(args, env);
}

function l_let(args: Types.Expr[], env: Types.Env): Types.LispValue {
  return (env[Eval.name(args[0])] = evaluate(args[1], env));
}

function l_bitand(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) & Eval.num(args[1], env);
}

function l_bitnot(args: Types.Expr[], env: Types.Env): number {
  return ~Eval.num(args[0], env);
}

function l_bitor(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) | Eval.num(args[1], env);
}

function l_bitxor(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) ^ Eval.num(args[1], env);
}

function l_shl(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) << Eval.num(args[1], env);
}

function l_shr(args: Types.Expr[], env: Types.Env): number {
  return Eval.num(args[0], env) >> Eval.num(args[1], env);
}

function l_if(args: Types.Expr[], env: Types.Env): Types.LispValue {
  return evaluate(args[0], env) ? evaluate(args[1], env) : evaluate(args[2], env);
}

function l_is_atom(args: Types.Expr[], env: Types.Env): number {
  return +(typeof evaluate(args[0], env) === 'number');
}

function l_is_lambda(args: Types.Expr[], env: Types.Env): number {
  return +(typeof evaluate(args[0], env) === 'function');
}

function l_loop(args: Types.Expr[], env: Types.Env): number {
  while (evaluate(args[0], env) === TRUE) evaluate(args[1], env);
  return -1;
}

function l_set(args: Types.Expr[], env: Types.Env): Types.LispValue[] {
  const array = Eval.arr(args[0], env);
  array[Eval.num(args[1], env)] = evaluate(args[2], env);
  return array;
}

function l_pop(args: Types.Expr[], env: Types.Env): Types.LispValue[] {
  const array = Eval.arr(args[0], env);
  array.pop();
  return array;
}

function l_write(args: Types.Expr[], env: Types.Env): Types.LispValue {
  args.forEach(a => process.stdout.write(String(evaluate(a, env))));
  return FALSE;
}

function l_lambda(args: Types.Expr[], env: Types.Env): Types.LispFn {
  const params = args.slice(0, -1);
  return (props: Types.Expr[] = [], scope: Types.Env) => {
    const localEnv: Types.Env = Object.create(env);
    props.forEach((prop, i) => (localEnv[Eval.name(params[i])] = evaluate(prop, scope)));
    return evaluate(args.at(-1)!, localEnv);
  };
}

const keywords: Types.Env = {
  ['mod']: l_mod,
  ['length']: l_length,
  ['/']: l_div,
  ['+']: l_add,
  ['*']: l_mul,
  ['-']: l_sub,
  ['array']: l_array,
  ['get']: l_get,
  ['do']: l_do,
  ['not']: l_not,
  ['=']: l_eq,
  ['<']: l_lt,
  ['>']: l_gt,
  ['>=']: l_gte,
  ['<=']: l_lte,
  ['and']: l_and,
  ['or']: l_or,
  ['apply']: l_apply,
  ['let']: l_let,
  ['&']: l_bitand,
  ['~']: l_bitnot,
  ['|']: l_bitor,
  ['^']: l_bitxor,
  ['<<']: l_shl,
  ['>>']: l_shr,
  ['if']: l_if,
  ['atom?']: l_is_atom,
  ['lambda?']: l_is_lambda,
  ['loop']: l_loop,
  ['set!']: l_set,
  ['pop!']: l_pop,
  ['write']: l_write,
  ['lambda']: l_lambda
};

function evaluate(exp: Types.Expr, env: Types.Env = keywords): Types.LispValue {
  if (!Array.isArray(exp) || exp.length === 0) return [];
  const [head, ...args] = isLeaf(exp) ? [exp] : (exp as [Types.Leaf, ...Types.Expr[]]);
  const [type, value] = head;
  return handlers[type]?.(value, args, env) ?? [];
}

export default function run(strings: TemplateStringsArray | string, ...values: unknown[]): Types.LispValue {
  if (typeof strings === 'string') {
    return evaluate(parse(`(apply (lambda (do ${strings})))`));
  }

  const source = (strings as TemplateStringsArray).reduce((acc: string, str: string, i: number) => {
    return acc + str + (values[i] !== undefined ? values[i] : '');
  }, '');

  return evaluate(parse(`(apply (lambda (do ${source})))`));
}
