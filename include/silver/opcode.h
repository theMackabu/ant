#ifdef OP_FMT
OP_FMT(none)
OP_FMT(u8)
OP_FMT(i8)
OP_FMT(u16)
OP_FMT(i16)
OP_FMT(u32)
OP_FMT(i32)
OP_FMT(atom)
OP_FMT(atom_u8)
OP_FMT(label)
OP_FMT(label8)
OP_FMT(loc)
OP_FMT(loc8)
OP_FMT(loc_atom)
OP_FMT(arg)
OP_FMT(const)
OP_FMT(const8)
OP_FMT(npop)
OP_FMT(var_ref)
#undef OP_FMT
#endif

#ifdef  OP_DEF
#ifndef op_def
#define op_def(name, size, n_pop, n_push, f) OP_DEF(name, size, n_pop, n_push, f)
#endif

OP_DEF(  INVALID,           1,   0,   0, none)
OP_DEF(  CONST,             5,   0,   1, const)     /* push constant pool[idx] */
OP_DEF(  CONST_I8,          2,   0,   1, i8)        /* push small integer */
OP_DEF(  CONST8,            2,   0,   1, const8)    /* push constant pool[u8 idx] */
OP_DEF(  UNDEF,             1,   0,   1, none)
OP_DEF(  NULL,              1,   0,   1, none)
OP_DEF(  TRUE,              1,   0,   1, none)
OP_DEF(  FALSE,             1,   0,   1, none)
OP_DEF(  THIS,              1,   0,   1, none)      /* push current 'this' */
OP_DEF(  GLOBAL,            1,   0,   1, none)      /* push 'globalThis' (js->global) */
OP_DEF(  OBJECT,            1,   0,   1, none)      /* push empty object {} */
OP_DEF(  ARRAY,             3,   0,   1, npop)      /* push array from stack items */
OP_DEF(  REGEXP,            1,   2,   1, none)      /* pattern flags -> regexp */
OP_DEF(  CLOSURE,           5,   0,   1, const)     /* push closure from func pool */

OP_DEF(  POP,               1,   1,   0, none)      /* a -> */
OP_DEF(  DUP,               1,   1,   2, none)      /* a -> a a */
OP_DEF(  DUP2,              1,   2,   4, none)      /* a b -> a b a b */
OP_DEF(  SWAP,              1,   2,   2, none)      /* a b -> b a */
OP_DEF(  ROT3L,             1,   3,   3, none)      /* x a b -> a b x */
OP_DEF(  ROT3R,             1,   3,   3, none)      /* a b x -> x a b */
OP_DEF(  NIP,               1,   2,   1, none)      /* a b -> b */
OP_DEF(  NIP2,              1,   3,   1, none)      /* a b c -> c */
OP_DEF(  INSERT2,           1,   2,   3, none)      /* obj a -> a obj a */
OP_DEF(  INSERT3,           1,   3,   4, none)      /* obj prop a -> a obj prop a */
OP_DEF(  SWAP_UNDER,        1,   3,   3, none)      /* a b c -> b a c */
OP_DEF(  ROT4_UNDER,        1,   4,   4, none)      /* a b c d -> c a b d */

OP_DEF(  GET_LOCAL,         3,   0,   1, loc)
OP_DEF(  PUT_LOCAL,         3,   1,   0, loc)       /* store, consume */
OP_DEF(  SET_LOCAL,         3,   1,   1, loc)       /* store, keep on stack */
OP_DEF(  GET_LOCAL8,        2,   0,   1, loc8)      /* short encoding */
OP_DEF(  PUT_LOCAL8,        2,   1,   0, loc8)
OP_DEF(  SET_LOCAL8,        2,   1,   1, loc8)
OP_DEF(  SET_LOCAL_UNDEF,   3,   0,   0, loc)       /* mark TDZ uninitialized */
OP_DEF(  GET_LOCAL_CHK,     7,   0,   1, loc_atom)  /* get + TDZ check (u16 slot, u32 atom) */
OP_DEF(  PUT_LOCAL_CHK,     7,   1,   0, loc_atom)  /* put + TDZ check (u16 slot, u32 atom) */

OP_DEF(  GET_ARG,           3,   0,   1, arg)
OP_DEF(  PUT_ARG,           3,   1,   0, arg)
OP_DEF(  SET_ARG,           3,   1,   1, arg)
OP_DEF(  REST,              3,   0,   1, u16)       /* collect rest params */

OP_DEF(  GET_UPVAL,         3,   0,   1, var_ref)
OP_DEF(  PUT_UPVAL,         3,   1,   0, var_ref)
OP_DEF(  SET_UPVAL,         3,   1,   1, var_ref)
OP_DEF(  CLOSE_UPVAL,       3,   0,   0, loc)       /* close upvalues >= loc */

OP_DEF(  GET_GLOBAL,        7,   0,   1, atom)      /* push global[atom] (atom + ic_idx:u16) */
OP_DEF(  GET_GLOBAL_UNDEF,  7,   0,   1, atom)      /* push undefined if missing (atom + ic_idx:u16) */
OP_DEF(  PUT_GLOBAL,        5,   1,   0, atom)      /* global[atom] = TOS */

OP_DEF(  GET_FIELD,         7,   1,   1, atom)      /* obj -> val (atom + ic_idx:u16) */
OP_DEF(  GET_FIELD2,        7,   1,   2, atom)      /* obj -> obj val (atom + ic_idx:u16) */
OP_DEF(  PUT_FIELD,         7,   2,   0, atom)      /* obj val -> (atom + ic_idx:u16) */
OP_DEF(  GET_ELEM,          1,   2,   1, none)      /* obj key -> val */
OP_DEF(  GET_ELEM2,         1,   2,   2, none)      /* obj key -> obj val */
OP_DEF(  PUT_ELEM,          1,   3,   0, none)      /* obj key val -> */
OP_DEF(  DEFINE_FIELD,      5,   2,   1, atom)      /* obj val -> obj (own prop) */
OP_DEF(  GET_LENGTH,        1,   1,   1, none)      /* obj -> length */

OP_DEF(  GET_FIELD_OPT,    5,   1,   1, atom)       /* null-safe obj -> val */
OP_DEF(  GET_ELEM_OPT,     1,   2,   1, none)       /* null-safe obj key -> val */

OP_DEF(  GET_PRIVATE,      1,   2,   1, none)       /* obj prop -> value */
OP_DEF(  PUT_PRIVATE,      1,   3,   0, none)       /* obj value prop -> */
OP_DEF(  DEF_PRIVATE,      1,   3,   1, none)       /* obj prop value -> obj */

OP_DEF(  GET_SUPER,         1,   1,   1, none)      /* obj -> super */
OP_DEF(  GET_SUPER_VAL,     1,   3,   1, none)      /* this obj prop -> value */
OP_DEF(  PUT_SUPER_VAL,     1,   4,   0, none)      /* this obj prop value -> */

OP_DEF(  ADD,               1,   2,   1, none)
OP_DEF(  SUB,               1,   2,   1, none)
OP_DEF(  MUL,               1,   2,   1, none)
OP_DEF(  DIV,               1,   2,   1, none)
OP_DEF(  ADD_NUM,           1,   2,   1, none)      /* numeric-only fast path */
OP_DEF(  SUB_NUM,           1,   2,   1, none)      /* numeric-only fast path */
OP_DEF(  MUL_NUM,           1,   2,   1, none)      /* numeric-only fast path */
OP_DEF(  DIV_NUM,           1,   2,   1, none)      /* numeric-only fast path */
OP_DEF(  MOD,               1,   2,   1, none)
OP_DEF(  EXP,               1,   2,   1, none)
OP_DEF(  NEG,               1,   1,   1, none)      /* unary minus */
OP_DEF(  UPLUS,             1,   1,   1, none)      /* unary plus (ToNumber) */
OP_DEF(  INC,               1,   1,   1, none)      /* +1 */
OP_DEF(  DEC,               1,   1,   1, none)      /* -1 */
OP_DEF(  POST_INC,          1,   1,   2, none)      /* -> old new */
OP_DEF(  POST_DEC,          1,   1,   2, none)      /* -> old new */
OP_DEF(  INC_LOCAL,         2,   0,   0, loc8)      /* locals[i]++ in-place */
OP_DEF(  DEC_LOCAL,         2,   0,   0, loc8)      /* locals[i]-- in-place */
OP_DEF(  ADD_LOCAL,         2,   1,   0, loc8)      /* locals[i] += TOS */

OP_DEF(  EQ,                1,   2,   1, none)      /* == (abstract) */
OP_DEF(  NE,                1,   2,   1, none)      /* != */
OP_DEF(  SEQ,               1,   2,   1, none)      /* === (strict, matches TOK_SEQ) */
OP_DEF(  SNE,               1,   2,   1, none)      /* !== (strict, matches TOK_SNE) */
OP_DEF(  LT,                1,   2,   1, none)
OP_DEF(  LE,                1,   2,   1, none)
OP_DEF(  GT,                1,   2,   1, none)
OP_DEF(  GE,                1,   2,   1, none)
OP_DEF(  INSTANCEOF,        3,   2,   1, u16)       /* l r -> bool (ic_idx:u16) */
OP_DEF(  IN,                1,   2,   1, none)
OP_DEF(  IS_NULLISH,        1,   1,   1, none)      /* TOS is null|undefined? */
OP_DEF(  IS_UNDEF_OR_NULL,  1,   1,   1, none)      /* same, for ?? chains */

OP_DEF(  BAND,              1,   2,   1, none)      /* & (matches TOK_AND) */
OP_DEF(  BOR,               1,   2,   1, none)      /* | (matches TOK_OR) */
OP_DEF(  BXOR,              1,   2,   1, none)      /* ^ (matches TOK_XOR) */
OP_DEF(  BNOT,              1,   1,   1, none)      /* ~ (matches TOK_TILDA) */
OP_DEF(  SHL,               1,   2,   1, none)      /* << */
OP_DEF(  SHR,               1,   2,   1, none)      /* >> (signed) */
OP_DEF(  USHR,              1,   2,   1, none)      /* >>> (matches TOK_ZSHR) */

OP_DEF(  NOT,               1,   1,   1, none)      /* ! */
OP_DEF(  TYPEOF,            1,   1,   1, none)
OP_DEF(  VOID,              1,   1,   1, none)      /* eval + push undefined */
OP_DEF(  DELETE,            1,   2,   1, none)      /* obj key -> bool */
OP_DEF(  DELETE_VAR,        5,   0,   1, atom)      /* delete unqualified name */

OP_DEF(  JMP,               5,   0,   0, label)     /* unconditional */
OP_DEF(  JMP_FALSE,         5,   1,   0, label)     /* pop + branch if falsy */
OP_DEF(  JMP_TRUE,          5,   1,   0, label)     /* pop + branch if truthy */
OP_DEF(  JMP_FALSE_PEEK,    5,   1,   1, label)     /* peek + branch if falsy */
OP_DEF(  JMP_TRUE_PEEK,     5,   1,   1, label)     /* peek + branch if truthy */
OP_DEF(  JMP_NOT_NULLISH,   5,   1,   1, label)     /* peek + branch if NOT null/undefined */
OP_DEF(  JMP8,              2,   0,   0, label8)    /* short unconditional */
OP_DEF(  JMP_FALSE8,        2,   1,   0, label8)    /* short conditional */
OP_DEF(  JMP_TRUE8,         2,   1,   0, label8)    /* short conditional */

OP_DEF(  CALL,              3,   1,   1, npop)      /* func args... -> result */
OP_DEF(  CALL_METHOD,       3,   2,   1, npop)      /* this func args... -> result */
OP_DEF(  CALL_IS_PROTO,     3,   3,   1, u16)       /* this func arg -> bool (ic_idx:u16) */
OP_DEF(  TAIL_CALL,         3,   1,   0, npop)      /* tail-position call */
OP_DEF(  TAIL_CALL_METHOD,  3,   2,   0, npop)
OP_DEF(  NEW,               3,   2,   1, npop)      /* func new.target args -> obj */
OP_DEF(  APPLY,             3,   3,   1, u16)       /* func this [args] -> result */
OP_DEF(  NEW_APPLY,         3,   2,   1, u16)       /* func new.target [args] -> obj */
OP_DEF(  EVAL,              5,   1,   1, npop)      /* direct eval */
OP_DEF(  RETURN,            1,   1,   0, none)
OP_DEF(  RETURN_UNDEF,      1,   0,   0, none)
OP_DEF(  RETURN_ASYNC,      1,   1,   0, none)      /* return from async func */
OP_DEF(  CHECK_CTOR,        1,   0,   0, none)      /* verify called with new */
OP_DEF(  CHECK_CTOR_RET,    1,   1,   2, none)      /* validate constructor return */
OP_DEF(  HALT,              1,   0,   0, none)      /* stop execution */

OP_DEF(  THROW,             1,   1,   0, none)
OP_DEF(  THROW_ERROR,       6,   0,   0, atom_u8)   /* throw built-in error */
OP_DEF(  TRY_PUSH,          5,   0,   0, label)     /* push catch handler */
OP_DEF(  TRY_POP,           1,   0,   0, none)      /* pop catch handler */
OP_DEF(  CATCH,             5,   0,   1, label)     /* push caught value + finally addr */
OP_DEF(  FINALLY,           5,   0,   0, label)     /* enter finally block */
OP_DEF(  FINALLY_RET,       1,   1,   0, none)      /* return from finally */
OP_DEF(  NIP_CATCH,         1,   2,   1, none)      /* catch ... a -> a */

OP_DEF(  FOR_IN,            1,   1,   1, none)      /* obj -> iterator */
OP_DEF(  FOR_OF,            1,   1,   3, none)      /* iterable -> iter next catch_off */
OP_DEF(  FOR_AWAIT_OF,      1,   1,   3, none)      /* async iterable -> iter next catch_off */
OP_DEF(  ITER_NEXT,         2,   3,   5, u8)        /* advance iterator (u8 hint) */
OP_DEF(  ITER_GET_VALUE,    1,   2,   3, none)      /* catch_off obj -> catch_off value done */
OP_DEF(  ITER_CLOSE,        1,   3,   0, none)      /* close iterator */
OP_DEF(  ITER_CALL,         2,   4,   5, u8)        /* call iterator method */
OP_DEF(  AWAIT_ITER_NEXT,   1,   3,   4, none)      /* async iterator next */
OP_DEF(  DESTRUCTURE_INIT,  1,   1,   3, none)      /* iterable -> iter next tag */
OP_DEF(  DESTRUCTURE_NEXT,  1,   3,   4, none)      /* iter next tag -> iter next tag value|undef */
OP_DEF(  DESTRUCTURE_REST,  1,   3,   4, none)      /* iter next tag -> iter next tag array */
OP_DEF(  DESTRUCTURE_CLOSE, 1,   3,   0, none)      /* close destructuring iterator */

OP_DEF(  AWAIT,             1,   1,   1, none)      /* promise -> resolved value */
OP_DEF(  YIELD,             1,   1,   2, none)      /* val -> received */
OP_DEF(  YIELD_STAR_INIT,   3,   1,   0, loc)       /* iterable -> delegate locals */
OP_DEF(  YIELD_STAR_NEXT,   3,   1,   2, loc)       /* sent -> final value | suspend */
OP_DEF(  YIELD_STAR_THROW,  3,   1,   2, loc)       /* thrown -> final value | suspend */
OP_DEF(  YIELD_STAR_RETURN, 3,   1,   2, loc)       /* return value -> final value | suspend */
OP_DEF(  SPREAD,            1,   1,   0, none)      /* arr iterable -> arr */

OP_DEF(  DEFINE_METHOD,     6,   2,   1, atom_u8)   /* obj func -> obj (flags: get/set/static) */
OP_DEF(  DEFINE_METHOD_COMP,2,   3,   1, u8)        /* obj key func -> obj (computed name) */
OP_DEF(  SET_NAME,          5,   1,   1, atom)      /* set .name on function */
OP_DEF(  SET_NAME_COMP,     1,   2,   2, none)      /* set .name from computed key */
OP_DEF(  SET_PROTO,         1,   2,   1, none)      /* obj proto -> obj */
OP_DEF(  SET_HOME_OBJ,      1,   2,   2, none)      /* func home -> func home */
OP_DEF(  APPEND,            1,   3,   2, none)      /* append to array, update length */
OP_DEF(  COPY_DATA_PROPS,   2,   3,   3, u8)        /* Object.assign-like */

OP_DEF(  DEFINE_CLASS,      6,   2,   2, atom_u8)   /* parent ctor -> ctor proto */
OP_DEF(  DEFINE_CLASS_COMP, 6,   3,   3, atom_u8)   /* computed name variant */
OP_DEF(  ADD_BRAND,         1,   2,   0, none)      /* this_obj home_obj -> (private brand) */

OP_DEF(  TO_OBJECT,         1,   1,   1, none)      /* coerce to object wrapper */
OP_DEF(  TO_PROPKEY,        1,   1,   1, none)      /* coerce to string/symbol */
OP_DEF(  IS_UNDEF,          1,   1,   1, none)      /* TOS === undefined */
OP_DEF(  IS_NULL,           1,   1,   1, none)      /* TOS === null */

OP_DEF(  IMPORT,            1,   2,   1, none)      /* dynamic import(specifier) */
OP_DEF(  IMPORT_SYNC,       1,   1,   1, none)      /* sync module load for static import */
OP_DEF(  IMPORT_DEFAULT,    1,   1,   1, none)      /* ns -> default (SLOT_DEFAULT or ns) */
OP_DEF(  IMPORT_NAMED,      5,   1,   1, atom)      /* ns -> named export (throw if missing) */
OP_DEF(  EXPORT,            5,   1,   0, atom)      /* value -> (module namespace[name] = value) */
OP_DEF(  EXPORT_ALL,        1,   1,   0, none)      /* ns -> (export all properties) */

OP_DEF(  ENTER_WITH,        1,   1,   0, none)      /* enter with(obj) block */
OP_DEF(  EXIT_WITH,         1,   0,   0, none)      /* exit with(obj) block */

OP_DEF(  WITH_GET_VAR,      8,   0,   1, atom)      /* -> val (check with-obj then fallback) */
OP_DEF(  WITH_PUT_VAR,      8,   1,   0, atom)      /* val -> (check with-obj then fallback) */
OP_DEF(  WITH_DEL_VAR,      5,   0,   1, atom)      /* -> bool (delete from with-obj/global) */

OP_DEF(  SPECIAL_OBJ,       2,   0,   1, u8)        /* arguments, new.target, super, module import */
OP_DEF(  EMPTY,             1,   0,   1, none)      /* push T_EMPTY (array hole) */
OP_DEF(  DEBUGGER,          1,   0,   0, none)
OP_DEF(  NOP,               1,   0,   0, none)
OP_DEF(  PUT_CONST,         5,   1,   0, const)     /* constant pool[idx] = TOS */

op_def(  LABEL,             5,   0,   0, label)
op_def(  LINE_NUM,          5,   0,   0, u32)
op_def(  COL_NUM,           5,   0,   0, u32)

#undef OP_DEF
#undef op_def
#endif
