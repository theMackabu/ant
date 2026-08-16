#!/usr/bin/env ant

import { $ } from 'ant:shell';

const decoder = new TextDecoder();
const stderrText = result => decoder.decode(result.stderr);

const args = process.argv.slice(2);
const showAll = args.includes('--all');
const query = args.find(arg => arg !== '--all') || 'TODO';
const rootResult = await $`git rev-parse --show-toplevel`.nothrow();

if (rootResult.exitCode !== 0) {
  console.error('repo-radar must run inside a Git repository');
  process.exit(1);
}

const root = rootResult.text().trim();
const srcDir = `${root}/src`;
const includeDir = `${root}/include`;
const search = () => $`rg -n --color never --glob '*.{c,h,js,ts}' -- ${query} ${srcDir} ${includeDir}`.nothrow();
const relative = line => (line.startsWith(`${root}/`) ? line.slice(root.length + 1) : line);

if (showAll) {
  const result = await search();
  const output = result.text().trim();
  const results = output ? output.split('\n') : [];

  if (result.exitCode === 127) {
    console.error('ripgrep is not installed');
    process.exit(result.exitCode);
  }
  if (result.exitCode > 1) {
    const error = stderrText(result).trim();
    if (error) console.error(error);
    process.exit(result.exitCode);
  }

  for (const match of results) console.log(relative(match));
  process.exit(0);
}

const [branchResult, commitsResult, changesResult, filesResult, matchesResult] = await Promise.all([
  $`git -C ${root} branch --show-current`.nothrow(),
  $`git -C ${root} log -5 --pretty=format:%h%x09%ar%x09%s`.nothrow(),
  $`git -C ${root} status --short`.nothrow(),
  $`find ${srcDir} ${includeDir} -type f | wc -l`.nothrow(),
  search()
]);

const ansi = process.env.NO_COLOR === undefined;
const color = (code, value) => (ansi ? `\x1b[${code}m${value}\x1b[0m` : value);
const cyan = value => color('36', value);
const green = value => color('32', value);
const yellow = value => color('33', value);
const dim = value => color('2', value);
const bold = value => (ansi ? `\x1b[1m${value}\x1b[22m` : value);
const edge = cyan('│');

const branch = branchResult.text().trim() || 'detached HEAD';
const commitText = commitsResult.text().trim();
const commits = commitText ? commitText.split('\n') : [];
const changeText = changesResult.text().trim();
const changes = changeText ? changeText.split('\n') : [];
const sourceFiles = filesResult.text().trim() || '?';
const matchText = matchesResult.text().trim();
const matches = matchText ? matchText.split('\n') : [];

const width = 76;
const rule = (left, title = '', titleStyle = value => value, right = '─') => {
  const labelWidth = title ? title.length + 2 : 0;
  const label = title ? ` ${titleStyle(title)} ` : '';
  return `${left}${label}${right.repeat(Math.max(0, width - left.length - labelWidth))}`;
};
const field = (name, value) => {
  const label = `${name}:`.padEnd(14);
  console.log(`${edge} ${dim(label)}${value}`);
};

console.log(cyan(rule('╭─', 'ANT REPO RADAR', bold)));
field('repository', root.split('/').pop());
field('branch', green(branch));
field('source files', sourceFiles);
field('changed', changes.length === 0 ? green('clean') : yellow(`${changes.length} files`));
field('search', `${JSON.stringify(query)} · ${matches.length} matches`);

console.log(cyan(rule('├─', 'RECENT COMMITS')));
if (commits.length === 0) console.log(`${edge} ${dim('No commits found')}`);
for (const commit of commits) {
  const [hash = '', age = '', ...subjectParts] = commit.split('\t');
  const subject = subjectParts.join('\t');
  console.log(`${edge} ${yellow(hash.padEnd(10))}${dim(age.padEnd(18))}${subject}`);
}

console.log(cyan(rule('├─', `MATCHES FOR ${JSON.stringify(query)}`)));
if (matchesResult.exitCode === 127) {
  console.log(`${edge} ${yellow('ripgrep is not installed')}`);
} else if (matches.length === 0) {
  console.log(`${edge} ${dim('No matches')}`);
} else {
  for (const match of matches.slice(0, 8)) console.log(`${edge} ${relative(match)}`);
  if (matches.length > 8) console.log(`${edge} ${dim(`… ${matches.length - 8} more matches`)}`);
}

if (changes.length > 0) {
  console.log(cyan(rule('├─', 'WORKTREE')));
  for (const change of changes.slice(0, 6)) console.log(`${edge} ${change}`);
  if (changes.length > 6) console.log(`${edge} ${dim(`… ${changes.length - 6} more changed files`)}`);
}

console.log(cyan(rule('╰─')));
