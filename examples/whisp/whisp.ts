import { parse } from './parser';
import * as Types from './types';

const TRUE = 1;
const FALSE = 0;

const handlers: Record<Types.LispNode, (value: any, args: Types.Expr[], env: Types.Env) => Types.LispValue> = {
  [Types.LispNode.ATOM]: value => value,
  [Types.LispNode.STRING]: value => value,
  [Types.LispNode.WORD]: (value, _, env) => l_resolve(value, env).value as Types.LispValue,

  [Types.LispNode.APPLY]: (value, args, env) => {
    const { target, value: fn } = l_resolve(value, env);
    return target === env ? (fn as Types.LispFn)(args, env) : fn.call(target, ...args.map(a => evaluate(a, env)));
  }
};

const Eval = {
  name: (exp: Types.Expr) => (exp as Types.Leaf)[1],
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
  while (evaluate(args[0], env)) evaluate(args[1], env);
  return -1;
}

function l_set(args: Types.Expr[], env: Types.Env): Types.LispValue[] {
  const array = Eval.arr(args[0], env);
  array[Eval.num(args[1], env)] = evaluate(args[2], env);
  return array;
}

function l_push(args: Types.Expr[], env: Types.Env): Types.LispValue[] {
  const array = Eval.arr(args[0], env);
  array.push(evaluate(args[1], env));
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

function l_neq(args: Types.Expr[], env: Types.Env): number {
  return +(evaluate(args[0], env) !== evaluate(args[1], env));
}

function l_concat(args: Types.Expr[], env: Types.Env): string {
  return args.map(a => String(evaluate(a, env))).join('');
}

function l_resolve(name: string, env: Types.Env) {
  if (name in env) return { target: env, value: env[name] };
  if (name.includes('.')) return jsResolve(name);
  return { target: env, value: env[name] };
}

function l_dot(args: Types.Expr[], env: Types.Env): Types.LispValue {
  const obj: any = evaluate(args[0], env);
  const member = String(Eval.name(args[1]));
  const val = obj[member];
  if (typeof val === 'function') return val.call(obj, ...args.slice(2).map(a => evaluate(a, env)));
  return val as Types.LispValue;
}

function l_mut(args: Types.Expr[], env: Types.Env): Types.LispValue {
  const name = String(Eval.name(args[0]));
  const val = evaluate(args[1], env);
  let scope: any = env;
  while (scope && !Object.prototype.hasOwnProperty.call(scope, name)) {
    scope = Object.getPrototypeOf(scope);
  }
  if (scope) scope[name] = val;
  else env[name] = val;
  return val;
}

function l_cond(args: Types.Expr[], env: Types.Env): Types.LispValue {
  for (let i = 0; i < args.length - 1; i += 2) {
    if (evaluate(args[i], env)) return evaluate(args[i + 1], env);
  }
  if (args.length % 2 === 1) return evaluate(args.at(-1)!, env);
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

function l_fn(args: Types.Expr[], env: Types.Env): Types.LispValue {
  const name = Eval.name(args[0]);
  const paramList = args[1] as Types.Expr[];
  const bodyExprs = args.slice(2);
  const fn: Types.LispFn = (props: Types.Expr[] = [], scope: Types.Env) => {
    const localEnv: Types.Env = Object.create(env);
    paramList.forEach((param, i) => (localEnv[Eval.name(param)] = evaluate(props[i], scope)));
    return bodyExprs.reduce<Types.LispValue>((_, x) => evaluate(x, localEnv), 0);
  };
  return (env[name] = fn);
}

function jsResolve(path: string): { target: any; value: any } {
  const parts = path.split('.');
  let target: any = globalThis;
  parts.slice(0, -1).forEach(p => (target = target[p]));
  return { target, value: target[parts.at(-1)!] };
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
  ['!=']: l_neq,
  ['set!']: l_set,
  ['push!']: l_push,
  ['pop!']: l_pop,
  ['mut!']: l_mut,
  ['write']: l_write,
  ['concat']: l_concat,
  ['cond']: l_cond,
  ['.']: l_dot,
  ['lambda']: l_lambda,
  ['fn']: l_fn
};

function evaluate(exp: Types.Expr, env: Types.Env = keywords): Types.LispValue {
  if (!Array.isArray(exp) || exp.length === 0) return [];
  const [head, ...args] = isLeaf(exp) ? [exp] : (exp as [Types.Leaf, ...Types.Expr[]]);
  const [type, value] = head;
  return handlers[type]?.(value, args, env) ?? [];
}

export default function run(strings: TemplateStringsArray | string, ...values: unknown[]): Types.LispValue {
  let source: string;
  if (typeof strings === 'string') source = strings;
  else source = (strings as TemplateStringsArray).reduce((acc, str, i) => acc + str + (values[i] ?? ''), '');
  return evaluate(parse(`(do ${source})`), Object.create(keywords));
}
