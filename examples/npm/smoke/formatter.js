export function printTable(rows) {
  const normalize = r => r?.map(v => (v != null ? (typeof v === 'object' ? JSON.stringify(v) : String(v)) : ''));
  const normalized = rows.map(normalize);

  const widths = [0, 0];
  for (const r of normalized) if (r) for (let i = 0; i < 2; i++) widths[i] = Math.max(widths[i], r[i]?.length || 0);

  const totalWidth = widths[0] + widths[1] + 3;
  const border = (l, m, r) => l + '─'.repeat(widths[0] + 2) + m + '─'.repeat(widths[1] + 2) + r;
  const lines = [border('┌', '┬', '┐')];
  let lastRowSpanned = false;

  for (let i = 0; i < normalized.length; i++) {
    const r = normalized[i];
    lastRowSpanned = !r || !r[0] || !r[1];
    if (!r) lines.push('│' + ' '.repeat(totalWidth + 2) + '│');
    else if (!r[0]) lines.push('│ ' + r[1].padStart(totalWidth) + ' │');
    else if (!r[1]) lines.push('│ ' + r[0].padEnd(totalWidth) + ' │');
    else lines.push('│ ' + r[0].padEnd(widths[0]) + ' │ ' + r[1].padEnd(widths[1]) + ' │');
    if (i === 0) lines.push(border('├', '┼', '┤'));
  }

  lines.push(lastRowSpanned ? '└' + '─'.repeat(totalWidth + 2) + '┘' : border('└', '┴', '┘'));
  process.stdout.write(lines.join('\n') + '\n');
}
