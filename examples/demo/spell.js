// spelling corrector; see https://norvig.com/spell-correct.html

import * as readline from 'node:readline';

const CORPUS_URL = 'https://www.norvig.com/big.txt';
const LETTERS = 'abcdefghijklmnopqrstuvwxyz';

function words(text) {
  return text.toLowerCase().match(/\w+/g) ?? [];
}

function countWords(items) {
  const counts = new Map();

  for (const word of items) {
    counts.set(word, (counts.get(word) ?? 0) + 1);
  }

  return counts;
}

function sumCounts(counts) {
  let total = 0;
  for (const count of counts.values()) total += count;
  return total;
}

function probability(word, wordCounts, totalWordCount = sumCounts(wordCounts)) {
  return (wordCounts.get(word) ?? 0) / totalWordCount;
}

function mostCommon(wordCounts, limit) {
  return [...wordCounts].sort((left, right) => right[1] - left[1]).slice(0, limit);
}

function known(items, wordCounts) {
  const result = new Set();

  for (const word of items) {
    if (wordCounts.has(word)) result.add(word);
  }

  return result;
}

function edits1(word) {
  const edits = new Set();

  for (let i = 0; i <= word.length; i++) {
    const left = word.slice(0, i);
    const right = word.slice(i);

    if (right) edits.add(left + right.slice(1));
    if (right.length > 1) edits.add(left + right[1] + right[0] + right.slice(2));

    if (right) {
      for (const letter of LETTERS) {
        edits.add(left + letter + right.slice(1));
      }
    }

    for (const letter of LETTERS) {
      edits.add(left + letter + right);
    }
  }

  return edits;
}

function candidates(word, wordCounts) {
  const unchanged = known([word], wordCounts);
  if (unchanged.size) return unchanged;

  const oneEditAway = known(edits1(word), wordCounts);
  if (oneEditAway.size) return oneEditAway;

  const twoEditsAway = new Set();
  for (const firstEdit of edits1(word)) {
    for (const secondEdit of edits1(firstEdit)) {
      if (wordCounts.has(secondEdit)) twoEditsAway.add(secondEdit);
    }
  }
  if (twoEditsAway.size) return twoEditsAway;

  return new Set([word]);
}

function correction(word, wordCounts) {
  let bestWord = word;
  let bestCount = -1;

  for (const candidate of candidates(word, wordCounts)) {
    const count = wordCounts.get(candidate) ?? 0;
    if (count > bestCount) {
      bestWord = candidate;
      bestCount = count;
    }
  }

  return bestWord;
}

function assertEqual(actual, expected, label) {
  if (JSON.stringify(actual) !== JSON.stringify(expected)) {
    throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }

  console.log(`✓ ${label}`);
}

function unitTests(wordCounts) {
  assertEqual(correction('speling', wordCounts), 'spelling', 'speling → spelling (insert)');
  assertEqual(correction('korrectud', wordCounts), 'corrected', 'korrectud → corrected (replace 2)');
  assertEqual(correction('bycycle', wordCounts), 'bicycle', 'bycycle → bicycle (replace)');
  assertEqual(correction('inconvient', wordCounts), 'inconvenient', 'inconvient → inconvenient (insert 2)');
  assertEqual(correction('arrainged', wordCounts), 'arranged', 'arrainged → arranged (delete)');
  assertEqual(correction('peotry', wordCounts), 'poetry', 'peotry → poetry (transpose)');
  assertEqual(correction('peotryy', wordCounts), 'poetry', 'peotryy → poetry (transpose + delete)');
  assertEqual(correction('word', wordCounts), 'word', 'word → word (known)');
  assertEqual(correction('quintessential', wordCounts), 'quintessential', 'quintessential → quintessential (unknown)');
  assertEqual(words('This is a TEST.'), ['this', 'is', 'a', 'test'], 'word tokenization');
  assertEqual(
    mostCommon(countWords(words('This is a test. 123; A TEST this is.')), 5),
    [
      ['this', 2],
      ['is', 2],
      ['a', 2],
      ['test', 2],
      ['123', 1]
    ],
    'word frequency counting'
  );
  assertEqual(wordCounts.size, 32198, 'dictionary contains 32,198 words');
  assertEqual(sumCounts(wordCounts), 1115585, 'corpus contains 1,115,585 words');
  assertEqual(
    mostCommon(wordCounts, 10),
    [
      ['the', 79809],
      ['of', 40024],
      ['and', 38312],
      ['to', 28765],
      ['in', 22023],
      ['a', 21124],
      ['that', 12512],
      ['he', 12401],
      ['was', 11410],
      ['it', 10681]
    ],
    'ten most common words'
  );
  assertEqual(wordCounts.get('the'), 79809, '"the" appears 79,809 times');
  assertEqual(probability('quintessential', wordCounts), 0, 'P(quintessential) = 0');

  const theProbability = probability('the', wordCounts);
  if (theProbability <= 0.07 || theProbability >= 0.08) {
    throw new Error(`the probability: expected 0.07 < P(the) < 0.08, got ${theProbability}`);
  }
  console.log('✓ 0.07 < P(the) < 0.08');

  return 'unit_tests pass';
}

console.log('Loading spelling corpus...');

const response = await fetch(CORPUS_URL);
if (!response.ok) {
  throw new Error(`Could not load ${CORPUS_URL}: ${response.status} ${response.statusText}`);
}

const wordCounts = countWords(words(await response.text()));

console.log(`Loaded ${wordCounts.size.toLocaleString()} words.`);
console.log('Enter a word to correct, "test" to run unit tests, or "exit" to quit.\n');

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  prompt: 'spell> ',
  historySize: 100,
  removeHistoryDuplicates: true
});

rl.prompt();

rl.on('line', input => {
  const command = input.trim().toLowerCase();

  if (command === 'exit' || command === 'quit' || command === 'q') {
    rl.close();
    return;
  }

  if (command === 'test') {
    try {
      console.log(unitTests(wordCounts));
    } catch (error) {
      console.error(`unit_tests failed: ${error.message}`);
    }
  } else if (!/^[a-z]+$/.test(command)) {
    console.log('Enter one word using the letters a-z.');
  } else {
    console.log(correction(command, wordCounts));
  }

  rl.prompt();
});
