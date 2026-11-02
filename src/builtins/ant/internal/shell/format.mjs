// @ant: compact_on_build
const INDENT = '    ';

function raw(word) {
  if (word.assign) return word.assign.raw ?? '';
  return word.raw ?? word.map(part => (part.t === 'lit' ? part.v : '')).join('');
}

function redirection(r) {
  if (r.heredoc) return `${r.fd !== null && r.fd !== 0 ? r.fd : ''}${r.op}${r.heredoc.delim}`;
  const target = raw(r.target);
  const fd = r.fdVar ? `{${r.fdVar}}` : r.fd !== null ? String(r.fd) : '';
  switch (r.op) {
    case '>&':
      return `${fd || '1'}>&${target}`;
    case '<&':
      return `${fd || '0'}<&${target}`;
    case '&>':
    case '&>>':
      return `${r.op} ${target}`;
    default:
      return `${fd}${r.op} ${target}`;
  }
}

function heredocs(node, out) {
  for (const r of node.redirs || []) if (r.heredoc) out.push(r.heredoc);
  if (node.t === 'pipeline') for (const cmd of node.cmds) heredocs(cmd, out);
  if (node.t === 'andor') {
    heredocs(node.first, out);
    for (const link of node.rest) heredocs(link.pipe, out);
  }
  return out;
}

function redirs(node) {
  const list = (node.redirs || []).map(redirection);
  return list.length ? ' ' + list.join(' ') : '';
}

function inline(list) {
  return list.items.map(item => andor(item.node, 0) + (item.async ? ' &' : '')).join('; ');
}

function block(list, depth, group) {
  const pad = INDENT.repeat(depth);
  const lines = [];
  list.items.forEach((item, index) => {
    const last = index === list.items.length - 1;
    let text = pad + andor(item.node, depth);
    const docs = heredocs(item.node, []);
    if (item.async) text += ' &';
    if (docs.length) {
      for (const doc of docs) text += `\n${doc.raw}${doc.delim}`;
      text += '\n';
    } else if (!item.async && !(group && last)) text += ';';
    lines.push(text);
  });
  return lines.join('\n');
}

function andor(node, depth) {
  if (node.t !== 'andor') return pipeline(node, depth);
  let text = pipeline(node.first, depth);
  for (const link of node.rest) text += ` ${link.op} ${pipeline(link.pipe, depth)}`;
  return text;
}

function pipeline(node, depth) {
  if (node.t !== 'pipeline') return command(node, depth);
  let text = node.cmds.map((cmd, i) => command(cmd, depth) + (i < node.cmds.length - 1 ? (node.errs[i] ? ' |&' : ' |') : '')).join(' ');
  if (node.bang) text = '! ' + text;
  if (node.time) text = 'time ' + text;
  return text;
}

function groupBody(list, depth) {
  return `{ \n${block(list, depth + 1, true)}\n${INDENT.repeat(depth)}}`;
}

function command(node, depth) {
  const pad = INDENT.repeat(depth);
  switch (node.t) {
    case 'simple': {
      const parts = [...node.assigns.map(a => a.raw ?? ''), ...node.words.map(raw)];
      return parts.join(' ') + redirs(node);
    }
    case 'group':
      return groupBody(node.body, depth) + redirs(node);
    case 'subshell':
      return `( ${inline(node.body)} )` + redirs(node);
    case 'if': {
      let text = '';
      node.clauses.forEach((clause, index) => {
        const level = depth + index;
        const p = INDENT.repeat(level);
        text += `${index ? `\n${p}` : ''}if ${inline(clause.cond)}; then\n${block(clause.body, level + 1, false)}`;
        if (index < node.clauses.length - 1 || node.else) text += `\n${p}else`;
      });
      if (node.else) text += `\n${block(node.else, depth + node.clauses.length, false)}`;
      for (let index = node.clauses.length - 1; index >= 0; index--) {
        const p = INDENT.repeat(depth + index);
        text += `\n${p}fi${index > 0 ? ';' : ''}`;
      }
      return text + redirs(node);
    }
    case 'while':
      return `${node.until ? 'until' : 'while'} ${inline(node.cond)}; do\n${block(node.body, depth + 1, false)}\n${pad}done` + redirs(node);
    case 'for':
    case 'select': {
      const words = node.words === null ? '"$@"' : node.words.map(raw).join(' ');
      return `${node.t} ${node.name} in ${words};\n${pad}do\n${block(node.body, depth + 1, false)}\n${pad}done` + redirs(node);
    }
    case 'cfor':
      return `for ((${node.raw ?? ''}))\n${pad}do\n${block(node.body, depth + 1, false)}\n${pad}done` + redirs(node);
    case 'case': {
      let text = `case ${raw(node.word)} in \n`;
      for (const item of node.items) {
        text += `${INDENT.repeat(depth + 1)}${item.patterns.map(raw).join(' | ')})\n`;
        text += item.body.items.length ? `${block(item.body, depth + 2, true)}\n` : '\n';
        text += `${INDENT.repeat(depth + 1)}${item.term}\n`;
      }
      return text + `${pad}esac` + redirs(node);
    }
    case 'cond':
      return `[[ ${node.raw ?? ''} ]]` + redirs(node);
    case 'arithcmd':
      return `(( ${node.raw ?? ''} ))` + redirs(node);
    case 'func':
      return formatFunction(node.name, node.body, depth);
    case 'coproc':
      return node.text;
  }
  return '';
}

export function formatFunction(name, body, depth = 0) {
  const pad = INDENT.repeat(depth);
  const text = body.t === 'group' ? groupBody(body.body, depth) + redirs(body) : command(body, depth);
  return `${name} () \n${pad}${text}`;
}
