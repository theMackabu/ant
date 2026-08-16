import ops from 'ant:internal/shell_ops';

const QUOTE_NONE = 0;
const QUOTE_SINGLE = 1;
const QUOTE_DOUBLE = 2;

const REDIR_STDIN = 0;
const REDIR_STDOUT = 1;
const REDIR_STDOUT_APPEND = 2;
const REDIR_STDERR_TO_STDOUT = 3;

const TOKEN_EOF = 0;
const TOKEN_WORD = 1;
const TOKEN_PIPE = 2;
const TOKEN_AND = 3;
const TOKEN_OR = 4;
const TOKEN_NEWLINE = 5;
const TOKEN_SEMICOLON = 6;
const TOKEN_REDIR_STDIN = 7;
const TOKEN_REDIR_STDOUT = 8;
const TOKEN_REDIR_STDOUT_APPEND = 9;
const TOKEN_REDIR_STDERR_TO_STDOUT = 10;

const CONNECT_ALWAYS = 0;
const CONNECT_AND = 1;
const CONNECT_OR = 2;

const unsupportedKeywords = new Set([
  '!',
  '{',
  '}',
  'case',
  'do',
  'done',
  'elif',
  'else',
  'esac',
  'fi',
  'for',
  'function',
  'if',
  'in',
  'select',
  'then',
  'time',
  'until',
  'while'
]);

const compileCache = new WeakMap();
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const decoder = new TextDecoder();

function syntaxError(input, message) {
  throw new SyntaxError(`ant:shell: ${message} at template segment ${input.segment} offset ${input.offset}`);
}

class ShellInput {
  constructor(segments) {
    this.segments = segments;
    this.segment = 0;
    this.offset = 0;
    this.boundaryPending = false;
  }

  peek() {
    let segment = this.segment;
    let offset = this.offset;
    let boundaryPending = this.boundaryPending;
    for (;;) {
      if (segment >= this.segments.length) return { kind: 'eof' };
      const text = this.segments[segment];
      if (offset < text.length) return { kind: 'char', ch: text[offset] };
      if (segment + 1 < this.segments.length && !boundaryPending) {
        return { kind: 'interpolation', interpolation: segment };
      }
      segment++;
      offset = 0;
      boundaryPending = false;
    }
  }

  take() {
    for (;;) {
      if (this.segment >= this.segments.length) return { kind: 'eof' };
      const text = this.segments[this.segment];
      if (this.offset < text.length) {
        return { kind: 'char', ch: text[this.offset++] };
      }
      if (this.segment + 1 < this.segments.length && !this.boundaryPending) {
        this.boundaryPending = true;
        return { kind: 'interpolation', interpolation: this.segment };
      }
      this.segment++;
      this.offset = 0;
      this.boundaryPending = false;
    }
  }
}

function isSpace(ch) {
  return ch === ' ' || ch === '\t' || ch === '\r';
}

function isOperatorStart(ch) {
  return ch === '|' || ch === '&' || ch === ';' || ch === '\n' || ch === '<' || ch === '>';
}

function isNameStart(ch) {
  return ch === '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

function isNameContinue(ch) {
  return isNameStart(ch) || (ch >= '0' && ch <= '9');
}

function takeMatching(input, ch) {
  const event = input.peek();
  if (event.kind !== 'char' || event.ch !== ch) return false;
  input.take();
  return true;
}

function takeLineContinuation(input) {
  const segment = input.segment;
  const offset = input.offset;
  const boundaryPending = input.boundaryPending;
  const slash = input.take();
  const newline = input.take();
  if (slash.kind === 'char' && slash.ch === '\\' && newline.kind === 'char' && newline.ch === '\n') return true;
  input.segment = segment;
  input.offset = offset;
  input.boundaryPending = boundaryPending;
  return false;
}

function dollarExpansionError(input) {
  const next = input.peek();
  if (next.kind !== 'char') return null;
  if (next.ch === '(') {
    const segment = input.segment;
    const offset = input.offset;
    const boundaryPending = input.boundaryPending;
    input.take();
    const after = input.peek();
    input.segment = segment;
    input.offset = offset;
    input.boundaryPending = boundaryPending;
    return after.kind === 'char' && after.ch === '(' ? 'arithmetic expansion is not implemented' : 'command substitution is not implemented';
  }
  if (next.ch === '{' || isNameStart(next.ch) || (next.ch >= '0' && next.ch <= '9') || '*@#?-$!'.includes(next.ch)) {
    return 'parameter expansion is not implemented';
  }
  return null;
}

class ShellLexer {
  constructor(segments) {
    this.input = new ShellInput(segments);
  }

  word() {
    const parts = [];
    let literal = '';
    let quote = QUOTE_NONE;
    let literalQuote = QUOTE_NONE;
    let wordStarted = false;
    let keywordEligible = true;
    let assignmentWord = false;
    let assignmentCandidate = true;
    let assignmentNameLength = 0;

    const flush = () => {
      if (literal.length === 0) return;
      parts.push({ kind: 'literal', quote: literalQuote, text: literal });
      literal = '';
    };

    const disableAssignment = () => {
      if (!assignmentWord) assignmentCandidate = false;
    };

    const feedAssignment = ch => {
      if (assignmentWord || !assignmentCandidate) return;
      if (ch === '=') {
        assignmentWord = assignmentNameLength > 0;
        assignmentCandidate = false;
        return;
      }
      const valid = assignmentNameLength === 0 ? isNameStart(ch) : isNameContinue(ch);
      if (!valid) assignmentCandidate = false;
      else assignmentNameLength++;
    };

    for (;;) {
      const event = this.input.peek();
      if (event.kind === 'eof') break;

      if (event.kind === 'interpolation') {
        flush();
        this.input.take();
        parts.push({
          kind: 'interpolation',
          quote,
          interpolation: event.interpolation
        });
        wordStarted = true;
        keywordEligible = false;
        disableAssignment();
        literalQuote = quote;
        continue;
      }

      const ch = event.ch;
      if (quote === QUOTE_NONE && (isSpace(ch) || isOperatorStart(ch))) break;
      this.input.take();

      if (quote === QUOTE_SINGLE) {
        if (ch === "'") {
          flush();
          quote = QUOTE_NONE;
          literalQuote = quote;
        } else literal += ch;
        wordStarted = true;
        continue;
      }

      if (quote === QUOTE_DOUBLE) {
        if (ch === '"') {
          flush();
          quote = QUOTE_NONE;
          literalQuote = quote;
          wordStarted = true;
          continue;
        }
        if (ch === '\\') {
          const next = this.input.peek();
          if (next.kind === 'char' && (next.ch === '$' || next.ch === '`' || next.ch === '"' || next.ch === '\\' || next.ch === '\n')) {
            this.input.take();
            if (next.ch !== '\n') literal += next.ch;
            wordStarted = true;
            keywordEligible = false;
            disableAssignment();
            continue;
          }
        }
        if (ch === '$') {
          const message = dollarExpansionError(this.input);
          if (message) syntaxError(this.input, message);
        } else if (ch === '`') {
          syntaxError(this.input, 'command substitution is not implemented');
        }
        literal += ch;
        wordStarted = true;
        continue;
      }

      if (ch === "'") {
        flush();
        quote = QUOTE_SINGLE;
        literalQuote = quote;
        parts.push({ kind: 'literal', quote, text: '' });
        wordStarted = true;
        keywordEligible = false;
        disableAssignment();
        continue;
      }
      if (ch === '"') {
        flush();
        quote = QUOTE_DOUBLE;
        literalQuote = quote;
        parts.push({ kind: 'literal', quote, text: '' });
        wordStarted = true;
        keywordEligible = false;
        disableAssignment();
        continue;
      }
      if (ch === '\\') {
        keywordEligible = false;
        disableAssignment();
        const next = this.input.take();
        if (next.kind === 'eof') syntaxError(this.input, 'trailing backslash');
        if (next.kind === 'interpolation') {
          syntaxError(this.input, 'backslash cannot escape an interpolation boundary');
        }
        if (next.ch === '\n') continue;
        literal += next.ch;
        wordStarted = true;
        continue;
      }
      if (ch === '$') {
        const message = dollarExpansionError(this.input);
        if (message) syntaxError(this.input, message);
      } else if (ch === '`') {
        syntaxError(this.input, 'command substitution is not implemented');
      }
      feedAssignment(ch);
      literal += ch;
      wordStarted = true;
    }

    if (quote !== QUOTE_NONE) {
      syntaxError(this.input, quote === QUOTE_SINGLE ? 'unterminated single quote' : 'unterminated double quote');
    }
    flush();
    if (wordStarted && parts.length === 0) {
      parts.push({ kind: 'literal', quote: QUOTE_NONE, text: '' });
    }
    return {
      kind: TOKEN_WORD,
      word: parts,
      keywordEligible,
      assignmentWord
    };
  }

  next() {
    for (;;) {
      const event = this.input.peek();
      if (event.kind === 'eof') return { kind: TOKEN_EOF };
      if (event.kind === 'interpolation') return this.word();
      if (event.ch === '\\' && takeLineContinuation(this.input)) continue;
      if (isSpace(event.ch)) {
        this.input.take();
        continue;
      }
      if (event.ch === '#') {
        let consumed;
        do consumed = this.input.take();
        while (consumed.kind !== 'eof' && !(consumed.kind === 'char' && consumed.ch === '\n'));
        return consumed.kind === 'char' ? { kind: TOKEN_NEWLINE } : { kind: TOKEN_EOF };
      }
      break;
    }

    const event = this.input.peek();
    if (event.kind !== 'char') return this.word();
    if (event.ch === '\n') {
      this.input.take();
      return { kind: TOKEN_NEWLINE };
    }
    if (event.ch === ';') {
      this.input.take();
      return { kind: TOKEN_SEMICOLON };
    }
    if (event.ch === '|') {
      this.input.take();
      return { kind: takeMatching(this.input, '|') ? TOKEN_OR : TOKEN_PIPE };
    }
    if (event.ch === '&') {
      this.input.take();
      if (takeMatching(this.input, '&')) return { kind: TOKEN_AND };
      syntaxError(this.input, "background execution with '&' is not implemented");
    }
    if (event.ch === '<') {
      this.input.take();
      return { kind: TOKEN_REDIR_STDIN };
    }
    if (event.ch === '>') {
      this.input.take();
      return {
        kind: takeMatching(this.input, '>') ? TOKEN_REDIR_STDOUT_APPEND : TOKEN_REDIR_STDOUT
      };
    }

    if (event.ch === '2') {
      const segment = this.input.segment;
      const offset = this.input.offset;
      const boundaryPending = this.input.boundaryPending;
      this.input.take();
      if (takeMatching(this.input, '>') && takeMatching(this.input, '&') && takeMatching(this.input, '1')) {
        return { kind: TOKEN_REDIR_STDERR_TO_STDOUT };
      }
      this.input.segment = segment;
      this.input.offset = offset;
      this.input.boundaryPending = boundaryPending;
    }

    if (event.ch >= '0' && event.ch <= '9') {
      const segment = this.input.segment;
      const offset = this.input.offset;
      const boundaryPending = this.input.boundaryPending;
      let next;
      do this.input.take();
      while ((next = this.input.peek()).kind === 'char' && next.ch >= '0' && next.ch <= '9');
      if (next.kind === 'char' && (next.ch === '<' || next.ch === '>')) {
        syntaxError(this.input, 'numeric file descriptor redirection is not implemented');
      }
      this.input.segment = segment;
      this.input.offset = offset;
      this.input.boundaryPending = boundaryPending;
    }

    return this.word();
  }
}

function literalWord(word) {
  let text = '';
  for (const part of word) {
    if (part.kind !== 'literal') return null;
    text += part.text;
  }
  return text;
}

function unsupportedKeyword(token) {
  if (!token.keywordEligible || token.word.length !== 1) return null;
  const part = token.word[0];
  if (part.kind !== 'literal' || part.quote !== QUOTE_NONE) return null;
  return unsupportedKeywords.has(part.text) ? part.text : null;
}

function parseTemplate(segments) {
  const lexer = new ShellLexer(segments);
  let token = lexer.next();
  while (token.kind === TOKEN_NEWLINE) token = lexer.next();
  let connector = CONNECT_ALWAYS;
  const clauses = [];

  while (token.kind !== TOKEN_EOF) {
    const commands = [];
    for (;;) {
      const words = [];
      const redirections = [];
      let haveContent = false;

      for (;;) {
        if (token.kind === TOKEN_WORD) {
          if (words.length === 0 && token.assignmentWord) {
            syntaxError(lexer.input, 'variable assignments are not implemented');
          }
          const keyword = words.length === 0 ? unsupportedKeyword(token) : null;
          if (keyword) {
            syntaxError(lexer.input, `compound command '${keyword}' is not implemented`);
          }
          words.push(token.word);
          haveContent = true;
          token = lexer.next();
          continue;
        }
        if (token.kind === TOKEN_REDIR_STDERR_TO_STDOUT) {
          redirections.push({ kind: REDIR_STDERR_TO_STDOUT });
          haveContent = true;
          token = lexer.next();
          continue;
        }
        if (token.kind === TOKEN_REDIR_STDIN || token.kind === TOKEN_REDIR_STDOUT || token.kind === TOKEN_REDIR_STDOUT_APPEND) {
          const kind = token.kind === TOKEN_REDIR_STDIN ? REDIR_STDIN : token.kind === TOKEN_REDIR_STDOUT ? REDIR_STDOUT : REDIR_STDOUT_APPEND;
          token = lexer.next();
          if (token.kind !== TOKEN_WORD) {
            syntaxError(lexer.input, 'redirection requires a target word');
          }
          redirections.push({ kind, target: token.word });
          haveContent = true;
          token = lexer.next();
          continue;
        }
        break;
      }

      if (!haveContent) syntaxError(lexer.input, 'expected a command');
      commands.push({ words, redirections });
      if (token.kind !== TOKEN_PIPE) break;
      token = lexer.next();
      if (
        token.kind === TOKEN_EOF ||
        token.kind === TOKEN_NEWLINE ||
        token.kind === TOKEN_SEMICOLON ||
        token.kind === TOKEN_AND ||
        token.kind === TOKEN_OR ||
        token.kind === TOKEN_PIPE
      ) {
        syntaxError(lexer.input, "pipeline requires a command after '|'");
      }
    }

    clauses.push({ connector, commands });
    if (token.kind === TOKEN_AND) connector = CONNECT_AND;
    else if (token.kind === TOKEN_OR) connector = CONNECT_OR;
    else if (token.kind === TOKEN_NEWLINE || token.kind === TOKEN_SEMICOLON) {
      connector = CONNECT_ALWAYS;
    } else if (token.kind === TOKEN_EOF) break;
    else syntaxError(lexer.input, 'unexpected token after command');

    token = lexer.next();
    if (connector === CONNECT_ALWAYS) {
      while (token.kind === TOKEN_NEWLINE) token = lexer.next();
      if (token.kind === TOKEN_EOF) break;
    } else if (
      token.kind === TOKEN_EOF ||
      token.kind === TOKEN_NEWLINE ||
      token.kind === TOKEN_SEMICOLON ||
      token.kind === TOKEN_AND ||
      token.kind === TOKEN_OR
    ) {
      syntaxError(lexer.input, 'conditional operator requires a following command');
    }
  }
  return clauses;
}

function wordExpression(word) {
  if (word.length === 0) return '""';
  const terms = [];
  for (const part of word) {
    terms.push(part.kind === 'literal' ? JSON.stringify(part.text) : `String(__values[${part.interpolation}])`);
  }
  return terms.join('+');
}

function emitWord(source, word) {
  const literal = literalWord(word);
  if (literal !== null) {
    source.push(`__arg(__exec,${JSON.stringify(literal)});`);
    return;
  }
  if (word.length === 1 && word[0].kind === 'interpolation' && word[0].quote === QUOTE_NONE) {
    const index = word[0].interpolation;
    source.push(`__wordValue=__values[${index}];`);
    source.push('if(Array.isArray(__wordValue)){');
    source.push('for(__wordIndex=0;__wordIndex<__wordValue.length;__wordIndex++){');
    source.push('__arg(__exec,String(__wordValue[__wordIndex]));}}else{');
    source.push('__arg(__exec,String(__wordValue));}');
    return;
  }
  source.push(`__arg(__exec,${wordExpression(word)});`);
}

function compileCommand(source, command, commandIndex, commandCount) {
  const staticWords = [];
  let allStatic = command.words.length <= 32;
  for (const word of command.words) {
    const literal = literalWord(word);
    staticWords.push(literal);
    if (literal === null) allStatic = false;
  }
  if (!allStatic) {
    for (const word of command.words) emitWord(source, word);
  }

  for (const redirection of command.redirections) {
    const target = redirection.kind === REDIR_STDERR_TO_STDOUT ? 'null' : wordExpression(redirection.target);
    source.push(`__redirect(__exec,__ctx,${redirection.kind},${target},` + `${commandIndex},${commandCount});`);
  }

  source.push(`__command(__exec,__ctx,${commandCount}`);
  if (allStatic) {
    for (const word of staticWords) source.push(`,${JSON.stringify(word)}`);
  }
  source.push(');');
}

function compileProgram(clauses) {
  const source = [];
  if (clauses.length === 0) source.push('return __finish(__ctx,null);');
  else {
    source.push('let __exec,__result,__wordValue,__wordIndex;');
    for (const clause of clauses) {
      if (clause.connector === CONNECT_AND) {
        source.push('if(__result.exitCode===0){');
      } else if (clause.connector === CONNECT_OR) {
        source.push('if(__result.exitCode!==0){');
      }
      source.push('__exec=__begin(__ctx);');
      for (let i = 0; i < clause.commands.length; i++) {
        compileCommand(source, clause.commands[i], i, clause.commands.length);
      }
      source.push('__result=await __submit(__ctx,__exec);');
      if (clauses.length > 1) {
        source.push('if(__result.exited)return __finish(__ctx,__result);');
      }
      if (clause.connector !== CONNECT_ALWAYS) source.push('}');
    }
    source.push(clauses.length === 1 ? 'return __result;' : 'return __finish(__ctx,__result);');
  }

  const text = source.join('');
  return {
    source: text,
    execute: new AsyncFunction('__begin', '__arg', '__command', '__redirect', '__submit', '__finish', '__ctx', '__values', text)
  };
}

function debugPlan(clauses) {
  const quoteNames = ['none', 'single', 'double'];
  const connectorNames = ['always', 'and', 'or'];
  const redirectNames = ['stdin', 'stdout', 'stdout-append', 'stderr-to-stdout'];

  return clauses.map(clause => ({
    connector: connectorNames[clause.connector],
    commands: clause.commands.map(command => ({
      words: command.words.map(word =>
        word.map(part =>
          part.kind === 'literal'
            ? { literal: part.text, quote: quoteNames[part.quote] }
            : { interpolation: part.interpolation, quote: quoteNames[part.quote] }
        )
      ),
      redirections: command.redirections.map(redirection => {
        const result = { kind: redirectNames[redirection.kind] };
        if (redirection.kind !== REDIR_STDERR_TO_STDOUT) {
          result.target = redirection.target.map(part =>
            part.kind === 'literal'
              ? { literal: part.text, quote: quoteNames[part.quote] }
              : { interpolation: part.interpolation, quote: quoteNames[part.quote] }
          );
        }
        return result;
      })
    }))
  }));
}

function debugValue(value, depth = 0) {
  if (typeof value === 'string') return JSON.stringify(value);
  if (value === null) return 'null';
  if (value === undefined) return 'undefined';
  if (typeof value === 'number' || typeof value === 'boolean') return String(value);
  if (typeof value === 'bigint') return '<bigint>';
  if (typeof value === 'symbol') return '<symbol>';
  if (typeof value === 'function') return '<function>';
  if (Array.isArray(value)) {
    if (depth >= 4) return '<array>';
    const shown = value.slice(0, 32).map(item => debugValue(item, depth + 1));
    if (value.length > 32) shown.push(`... ${value.length - 32} more`);
    return `[${shown.join(', ')}]`;
  }
  return '<object>';
}

function debugCompile(source, plan) {
  console.error(`[shell:compile] JavaScript (${source.length} bytes)\n${source}\n` + `[shell:compile] __plan (${plan.length} bytes)\n${plan}`);
}

function debugInvoke(context, values, clauseCount) {
  console.error(
    '[shell:invoke] bindings\n' +
      '__begin = [native sh_runtime_begin]\n' +
      '__arg = [native sh_runtime_arg]\n' +
      '__command = [native sh_runtime_command]\n' +
      '__redirect = [native sh_runtime_redirect]\n' +
      '__submit = [native sh_runtime_submit]\n' +
      '__finish = [native sh_runtime_finish]\n' +
      `__ctx = { cwd: ${debugValue(context.cwd)}, accumulator: ` +
      `${clauseCount === 1 ? 'false' : 'true'} }\n` +
      `__values = ${debugValue(values)}\n` +
      `__plan = [JavaScript shell plan: ${clauseCount} ` +
      `clause${clauseCount === 1 ? '' : 's'}]`
  );
}

function shellOutput(result) {
  if (!result || !(result.stdout instanceof Uint8Array) || !(result.stderr instanceof Uint8Array)) {
    throw new TypeError('Invalid shell output');
  }
  Object.setPrototypeOf(result, ShellOutput.prototype);
  delete result.exited;
  return result;
}

class ShellOutput {
  constructor() {
    throw new TypeError('Illegal constructor');
  }

  text() {
    return decoder.decode(this.stdout);
  }

  json() {
    return JSON.parse(this.text());
  }

  arrayBuffer() {
    return this.stdout.buffer;
  }

  bytes() {
    return new Uint8Array(this.stdout.buffer, this.stdout.byteOffset, this.stdout.byteLength);
  }

  blob() {
    return new Blob([this.stdout]);
  }

  lines() {
    const lines = this.text().split('\n');
    if (lines.length && lines[lines.length - 1] === '') lines.pop();
    return lines;
  }
}

class ShellPromise {
  constructor(rawPromise) {
    const state = { nothrow: false, promise: null };
    state.promise = rawPromise.then(result => {
      const output = shellOutput(result);
      if (output.exitCode === 0 || state.nothrow) return output;
      const error = new Error(`Shell command failed with exit code ${output.exitCode}`);
      error.exitCode = output.exitCode;
      error.stdout = output.stdout;
      error.stderr = output.stderr;
      throw error;
    });
    this.state = state;
  }

  then(onFulfilled, onRejected) {
    return this.state.promise.then(onFulfilled, onRejected);
  }

  catch(onRejected) {
    return this.state.promise.catch(onRejected);
  }

  finally(onFinally) {
    return this.state.promise.finally(onFinally);
  }

  nothrow() {
    this.state.nothrow = true;
    return this;
  }

  text() {
    return this.state.promise.then(output => output.text());
  }

  json() {
    return this.state.promise.then(output => output.json());
  }

  arrayBuffer() {
    return this.state.promise.then(output => output.arrayBuffer());
  }

  bytes() {
    return this.state.promise.then(output => output.bytes());
  }

  blob() {
    return this.state.promise.then(output => output.blob());
  }

  lines() {
    return this.state.promise.then(output => output.lines());
  }
}

Object.defineProperty(ShellOutput, 'name', { value: 'ShellOutput' });
Object.defineProperty(ShellPromise, 'name', { value: 'ShellPromise' });

function templateSegments(strings) {
  if (!Array.isArray(strings)) {
    throw new TypeError('$() must be used as a tagged template');
  }
  const segments = Array.isArray(strings.raw) ? strings.raw : strings;
  if (segments.length === 0) {
    throw new TypeError('$() must be used as a tagged template');
  }
  for (const segment of segments) {
    if (typeof segment !== 'string') {
      throw new TypeError('$() must be used as a tagged template');
    }
  }
  return segments;
}

export function $(strings, ...values) {
  const segments = templateSegments(strings);
  let compiled = compileCache.get(strings);
  if (!compiled) {
    const clauses = parseTemplate(segments);
    const lowered = compileProgram(clauses);
    compiled = {
      execute: lowered.execute,
      source: lowered.source,
      clauseCount: clauses.length,
      plan: ops.debugEnabled ? JSON.stringify(debugPlan(clauses)) : ''
    };
    compileCache.set(strings, compiled);
    if (ops.debugEnabled) debugCompile(compiled.source, compiled.plan);
  }

  const context = ops.context(compiled.clauseCount !== 1);
  if (ops.debugEnabled) debugInvoke(context, values, compiled.clauseCount);
  const rawPromise = compiled.execute(ops.begin, ops.arg, ops.command, ops.redirect, ops.submit, ops.finish, context, values);
  return new ShellPromise(rawPromise);
}

export default { $ };
