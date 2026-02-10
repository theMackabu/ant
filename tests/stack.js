const STACK_SIZE = 256;
const PROGRAM_SIZE = 1024;

const Opcode = {
  OP_PUSH: 0,
  OP_POP: 1,
  OP_ADD: 2,
  OP_SUB: 3,
  OP_MUL: 4,
  OP_DIV: 5,
  OP_PRINT: 6,
  OP_JMP: 7,
  OP_JZ: 8,
  OP_LT: 9,
  OP_EQ: 10,
  OP_NEG: 11,
  OP_HALT: 12
};

const opcodeNames = {
  [Opcode.OP_PUSH]: 'OP_PUSH',
  [Opcode.OP_POP]: 'OP_POP',
  [Opcode.OP_ADD]: 'OP_ADD',
  [Opcode.OP_SUB]: 'OP_SUB',
  [Opcode.OP_MUL]: 'OP_MUL',
  [Opcode.OP_DIV]: 'OP_DIV',
  [Opcode.OP_PRINT]: 'OP_PRINT',
  [Opcode.OP_JMP]: 'OP_JMP',
  [Opcode.OP_JZ]: 'OP_JZ',
  [Opcode.OP_LT]: 'OP_LT',
  [Opcode.OP_EQ]: 'OP_EQ',
  [Opcode.OP_NEG]: 'OP_NEG',
  [Opcode.OP_HALT]: 'OP_HALT'
};

// Debug settings
const DEBUG_ENABLED = true;

class VM {
  constructor() {
    this.sp = -1;
    this.pc = 0;
    this.stack = new Array(STACK_SIZE).fill(0);
    this.program = new Uint8Array(PROGRAM_SIZE);
  }

  free() {
    this.sp = -1;
    this.pc = 0;
  }

  checkStackHasValues(count) {
    if (this.sp < count - 1) {
      throw new Error(`Stack Underflow! Need ${count} value(s), but stack has ${this.sp + 1}`);
    }
  }

  push(value) {
    if (this.sp >= STACK_SIZE - 1) {
      throw new Error('Stack Overflow!');
    }
    this.sp++;
    this.stack[this.sp] = value;
    this.tracePush(value);
  }

  pop() {
    this.checkStackHasValues(1);
    const result = this.stack[this.sp];
    this.sp--;
    this.tracePop(result);
    return result;
  }

  performBinary(op) {
    this.checkStackHasValues(2);
    const b = this.pop();
    const a = this.pop();
    let result;
    switch (op) {
      case '+':
        result = a + b;
        break;
      case '-':
        result = a - b;
        break;
      case '*':
        result = a * b;
        break;
      case '/':
        result = Math.trunc(a / b);
        break;
    }
    this.push(result);
  }

  performUnary(op) {
    this.checkStackHasValues(1);
    const a = this.pop();
    let result;
    switch (op) {
      case '-':
        result = -a;
        break;
    }
    this.push(result);
  }

  execute() {
    this.traceHeader();

    while (true) {
      const op = this.program[this.pc];
      this.traceInstruction(this.pc, op);
      this.pc++;

      switch (op) {
        case Opcode.OP_PUSH: {
          const val = this.readInt64();
          this.push(val);
          break;
        }
        case Opcode.OP_POP:
          this.pop();
          break;
        case Opcode.OP_HALT:
          this.traceHalt();
          return;
        case Opcode.OP_PRINT:
          this.checkStackHasValues(1);
          console.log(this.stack[this.sp]);
          break;
        case Opcode.OP_ADD:
          this.performBinary('+');
          break;
        case Opcode.OP_SUB:
          this.performBinary('-');
          break;
        case Opcode.OP_MUL:
          this.performBinary('*');
          break;
        case Opcode.OP_DIV:
          this.performBinary('/');
          break;
        case Opcode.OP_NEG:
          this.performUnary('-');
          break;
        default:
          throw new Error('Unknown Opcode!');
      }

      this.printStack();
      this.traceSeparator();
    }
  }

  readInt64() {
    // Read 8 bytes as little-endian 64-bit integer
    // JS doesn't handle 64-bit ints natively, but for small values this works
    let val = 0;
    for (let i = 0; i < 8; i++) {
      val |= this.program[this.pc + i] << (i * 8);
    }
    this.pc += 8;
    return val;
  }

  // Debug/trace methods
  traceHeader() {
    if (!DEBUG_ENABLED) return;
    console.log('\n\x1b[96m\x1b[1m=== VM Execution Trace ===\x1b[0m\n');
  }

  traceInstruction(pc, opcode) {
    if (!DEBUG_ENABLED) return;
    const name = opcodeNames[opcode] || 'UNKNOWN';
    console.log(`\x1b[94m[PC=${pc}]\x1b[0m \x1b[32m\x1b[1m${name}\x1b[0m`);
  }

  tracePush(value) {
    if (!DEBUG_ENABLED) return;
    console.log(`\x1b[2m        \x1b[35mPUSH\x1b[0m \x1b[93m${value}\x1b[0m`);
  }

  tracePop(value) {
    if (!DEBUG_ENABLED) return;
    console.log(`\x1b[2m        \x1b[31mPOP \x1b[0m \x1b[93m${value}\x1b[0m`);
  }

  traceHalt() {
    if (!DEBUG_ENABLED) return;
    console.log('\n\x1b[91m\x1b[1m=== Execution Halted ===\x1b[0m');
  }

  traceSeparator() {
    if (!DEBUG_ENABLED) return;
    console.log();
  }

  printStack() {
    if (!DEBUG_ENABLED) return;
    let stackStr = this.stack.slice(0, this.sp + 1).join(' ');
    console.log(`\x1b[2m    Stack \x1b[36m[sp=${this.sp}]\x1b[0m: [ \x1b[33m${stackStr}\x1b[0m ]`);
  }
}

// Helper to write int64 to program
function writeInt(program, pc, val) {
  for (let i = 0; i < 8; i++) {
    program[pc.value] = (val >> (i * 8)) & 0xff;
    pc.value++;
  }
}

// Main
function main() {
  const v = new VM();
  const values = [10, 13, 6, 25, 42];
  const pc = { value: 0 };

  for (const val of values) {
    v.program[pc.value++] = Opcode.OP_PUSH;
    writeInt(v.program, pc, val);
  }

  v.program[pc.value++] = Opcode.OP_MUL;
  v.program[pc.value++] = Opcode.OP_ADD;
  v.program[pc.value++] = Opcode.OP_NEG;
  v.program[pc.value++] = Opcode.OP_DIV;
  v.program[pc.value++] = Opcode.OP_SUB;
  v.program[pc.value++] = Opcode.OP_PRINT;
  v.program[pc.value++] = Opcode.OP_HALT;

  v.execute();
  v.free();
}

main();
