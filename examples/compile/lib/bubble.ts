export interface BubbleOptions {
  width: number;
}

export function wrap(text: string, width: number): string[] {
  const words = text.split(/\s+/).filter(Boolean);
  const lines: string[] = [];
  let line = '';

  for (const word of words) {
    if (line && line.length + word.length + 1 > width) {
      lines.push(line);
      line = word;
    } else {
      line = line ? `${line} ${word}` : word;
    }
  }

  if (line) lines.push(line);
  return lines.length ? lines : [''];
}

export function bubble(text: string, opts: BubbleOptions): string {
  const lines = wrap(text, opts.width);
  const inner = Math.max(...lines.map(l => l.length));
  const top = ` ${'_'.repeat(inner + 2)}`;
  const bottom = ` ${'-'.repeat(inner + 2)}`;

  const body = lines.map((l, i) => {
    const pad = l.padEnd(inner);
    if (lines.length === 1) return `< ${pad} >`;
    if (i === 0) return `/ ${pad} \\`;
    if (i === lines.length - 1) return `\\ ${pad} /`;
    return `| ${pad} |`;
  });

  return [top, ...body, bottom].join('\n');
}
