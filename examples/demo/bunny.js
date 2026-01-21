const Opcode = {
  CONST: 0,
  LOAD: 1,
  ADD: 2,
  CALL: 3,
  RETURN: 4,
  EXTERN: 5,
  HALT: 6
};

const TokenType = {
  EOF: 0,
  FN: 1,
  RETURN: 2,
  IDENT: 3,
  NUMBER: 4,
  LPAREN: 5,
  RPAREN: 6,
  LBRACE: 7,
  RBRACE: 8,
  COMMA: 9,
  SEMICOLON: 10,
  PLUS: 11,
  DOT: 12
};

class Lexer {
  constructor(src) {
    this.src = src;
    this.pos = 0;
    this.current = this.nextToken();
  }

  skipWhitespace() {
    while (this.pos < this.src.length && /\s/.test(this.src[this.pos])) {
      this.pos++;
    }
  }

  nextToken() {
    this.skipWhitespace();

    if (this.pos >= this.src.length) {
      return { type: TokenType.EOF };
    }

    let c = this.src[this.pos];

    if (/[a-zA-Z]/.test(c)) {
      let start = this.pos;
      while (this.pos < this.src.length && /[a-zA-Z0-9_]/.test(this.src[this.pos])) {
        this.pos++;
      }
      let value = this.src.slice(start, this.pos);

      if (value === 'fn') return { type: TokenType.FN, value };
      if (value === 'return') return { type: TokenType.RETURN, value };
      return { type: TokenType.IDENT, value };
    }

    if (/[0-9]/.test(c)) {
      let start = this.pos;
      while (this.pos < this.src.length && /[0-9]/.test(this.src[this.pos])) {
        this.pos++;
      }
      return { type: TokenType.NUMBER, numValue: parseInt(this.src.slice(start, this.pos)) };
    }

    this.pos++;
    const charMap = {
      '(': TokenType.LPAREN,
      ')': TokenType.RPAREN,
      '{': TokenType.LBRACE,
      '}': TokenType.RBRACE,
      ',': TokenType.COMMA,
      ';': TokenType.SEMICOLON,
      '+': TokenType.PLUS,
      '.': TokenType.DOT
    };
    return { type: charMap[c] || TokenType.EOF };
  }

  advance() {
    this.current = this.nextToken();
  }
}

class VM {
  constructor() {
    this.code = [];
    this.entryPoint = 0;
    this.stack = [];
    this.callStack = [];
    this.functions = [];
    this.externs = [
      {
        name: 'std.io.println',
        fn: vm => {
          console.log(vm.pop());
          vm.push(0);
        },
        params: 1
      },
      {
        name: 'bunny.squeak',
        fn: vm => {
          console.log('squeak');
          vm.push(0);
        },
        params: 0
      }
    ];
    this.output = [];
  }

  emit(op, operand = 0) {
    this.code.push({ op, operand });
  }

  push(v) {
    this.stack.push(v);
  }
  pop() {
    return this.stack.pop();
  }

  findParam(func, name) {
    if (!func) return -1;
    let idx = func.paramNames.indexOf(name);
    return idx !== -1 ? func.params - 1 - idx : -1;
  }

  findFunction(name) {
    return this.functions.findIndex(f => f.name === name);
  }

  findExtern(name) {
    return this.externs.findIndex(e => e.name === name);
  }

  run() {
    let pc = this.entryPoint;
    let fp = 0;
    this.output = [];

    const origLog = console.log;
    console.log = (...args) => this.output.push(args.join(' '));

    while (pc < this.code.length) {
      const instr = this.code[pc];

      switch (instr.op) {
        case Opcode.CONST:
          this.push(instr.operand);
          pc++;
          break;

        case Opcode.LOAD:
          this.push(this.stack[fp + instr.operand]);
          pc++;
          break;

        case Opcode.ADD: {
          let b = this.pop(),
            a = this.pop();
          this.push(a + b);
          pc++;
          break;
        }

        case Opcode.CALL: {
          let func = this.functions[instr.operand];
          this.callStack.push(pc + 1, fp);
          fp = this.stack.length - func.params;
          pc = func.addr;
          break;
        }

        case Opcode.RETURN: {
          let ret = this.pop();
          this.stack.length = fp;
          fp = this.callStack.pop();
          pc = this.callStack.pop();
          this.push(ret);
          break;
        }

        case Opcode.EXTERN:
          this.externs[instr.operand].fn(this);
          pc++;
          break;

        case Opcode.HALT:
          console.log = origLog;
          return;
      }
    }
    console.log = origLog;
  }

  printBytecode() {
    const names = ['CONST', 'LOAD', 'ADD', 'CALL', 'RETURN', 'EXTERN', 'HALT'];
    let out = 'bytecode:\n';
    this.code.forEach((instr, i) => {
      out += `${String(i).padStart(3)}: ${names[instr.op].padEnd(8)} ${instr.operand}\n`;
    });
    return out;
  }
}

function parseQualifiedName(lex) {
  if (lex.current.type !== TokenType.IDENT) return null;

  let name = lex.current.value;
  lex.advance();

  while (lex.current.type === TokenType.DOT) {
    lex.advance();
    if (lex.current.type !== TokenType.IDENT) break;
    name += '.' + lex.current.value;
    lex.advance();
  }
  return name;
}

function parseExpr(lex, vm, currentFunc) {
  if (lex.current.type === TokenType.NUMBER) {
    vm.emit(Opcode.CONST, lex.current.numValue);
    lex.advance();
  } else if (lex.current.type === TokenType.IDENT) {
    let name = parseQualifiedName(lex);

    if (lex.current.type === TokenType.LPAREN) {
      parseCall(lex, vm, currentFunc, name);
    } else {
      let offset = vm.findParam(currentFunc, name);
      vm.emit(Opcode.LOAD, offset);
    }
  }

  if (lex.current.type === TokenType.PLUS) {
    lex.advance();
    parseExpr(lex, vm, currentFunc);
    vm.emit(Opcode.ADD);
  }
}

function parseCall(lex, vm, currentFunc, name) {
  lex.advance(); // skip (

  if (lex.current.type !== TokenType.RPAREN) {
    parseExpr(lex, vm, currentFunc);
    while (lex.current.type === TokenType.COMMA) {
      lex.advance();
      parseExpr(lex, vm, currentFunc);
    }
  }
  lex.advance(); // skip )

  let externId = vm.findExtern(name);
  if (externId !== -1) {
    vm.emit(Opcode.EXTERN, externId);
  } else {
    vm.emit(Opcode.CALL, vm.findFunction(name));
  }
}

function parseFunction(lex, vm) {
  lex.advance(); // skip 'fn'

  let funcName = lex.current.value;
  lex.advance();
  lex.advance(); // skip (

  let paramNames = [];
  if (lex.current.type === TokenType.IDENT) {
    paramNames.push(lex.current.value);
    lex.advance();
    while (lex.current.type === TokenType.COMMA) {
      lex.advance();
      paramNames.push(lex.current.value);
      lex.advance();
    }
  }
  lex.advance(); // skip )
  lex.advance(); // skip {

  let func = {
    name: funcName,
    addr: vm.code.length,
    params: paramNames.length,
    paramNames
  };
  vm.functions.push(func);

  while (lex.current.type !== TokenType.RBRACE) {
    if (lex.current.type === TokenType.RETURN) {
      lex.advance();
      parseExpr(lex, vm, func);
      vm.emit(Opcode.RETURN);
      lex.advance(); // skip ;
    }
  }
  lex.advance(); // skip }
}

function parseProgram(lex, vm) {
  while (lex.current.type === TokenType.FN) {
    parseFunction(lex, vm);
  }

  vm.entryPoint = vm.code.length;

  while (lex.current.type !== TokenType.EOF) {
    if (lex.current.type === TokenType.IDENT) {
      let name = parseQualifiedName(lex);

      if (lex.current.type === TokenType.LPAREN) {
        parseCall(lex, vm, null, name);
        if (lex.current.type === TokenType.SEMICOLON) lex.advance();
      }
    } else {
      lex.advance();
    }
  }

  vm.emit(Opcode.HALT);
}

function compile(source) {
  const vm = new VM();
  const lex = new Lexer(source);
  parseProgram(lex, vm);
  return vm;
}

const source = `
fn add(a, b) {
    return a + b;
}
bunny.squeak();
std.io.println(add(5, 10));
`.trim();

console.log('Source:\n');
console.log(source);
console.log('\n' + '='.repeat(40) + '\n');

const vm = compile(source);

console.log(vm.printBytecode());
console.log('Output:');
vm.run();
vm.output.forEach(line => console.log(line));
