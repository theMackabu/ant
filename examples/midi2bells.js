import { readFile } from 'node:fs';

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

function midiToNote(midi) {
  const octave = Math.floor(midi / 12) - 1;
  const name = NOTE_NAMES[midi % 12];
  return { name, octave, str: `${name}${octave}` };
}

function readVarLen(buf, pos) {
  let value = 0;
  let byte;
  do {
    byte = buf[pos++];
    value = (value << 7) | (byte & 0x7f);
  } while (byte & 0x80);
  return { value, pos };
}

function parseMidi(buf) {
  let pos = 0;

  const headerChunk = String.fromCharCode(buf[0], buf[1], buf[2], buf[3]);
  if (headerChunk !== 'MThd') throw new Error('Not a MIDI file');
  const format = (buf[8] << 8) | buf[9];
  const numTracks = (buf[10] << 8) | buf[11];
  const division = (buf[12] << 8) | buf[13];
  pos = 14;

  console.log(`Format: ${format}, Tracks: ${numTracks}, Division: ${division}`);

  const tracks = [];
  let tempo = 500000;

  for (let t = 0; t < numTracks; t++) {
    const chunkType = String.fromCharCode(buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]);
    pos += 4;
    const chunkLen = (buf[pos] << 24) | (buf[pos + 1] << 16) | (buf[pos + 2] << 8) | buf[pos + 3];
    pos += 4;

    if (chunkType !== 'MTrk') {
      pos += chunkLen;
      continue;
    }

    const endPos = pos + chunkLen;
    const events = [];
    let absTick = 0;
    let runningStatus = 0;
    let trackName = `Track ${t}`;

    while (pos < endPos) {
      const delta = readVarLen(buf, pos);
      absTick += delta.value;
      pos = delta.pos;

      let status = buf[pos];
      if (status < 0x80) {
        status = runningStatus;
      } else {
        runningStatus = status;
        pos++;
      }

      const type = status & 0xf0;
      const channel = status & 0x0f;

      if (type === 0x90) {
        const note = buf[pos++];
        const velocity = buf[pos++];
        if (velocity > 0) {
          events.push({ type: 'on', tick: absTick, note, velocity, channel });
        } else {
          events.push({ type: 'off', tick: absTick, note, channel });
        }
      } else if (type === 0x80) {
        const note = buf[pos++];
        pos++;
        events.push({ type: 'off', tick: absTick, note, channel });
      } else if (type === 0xa0 || type === 0xb0 || type === 0xe0) {
        pos += 2;
      } else if (type === 0xc0 || type === 0xd0) {
        pos += 1;
      } else if (status === 0xff) {
        const metaType = buf[pos++];
        const len = readVarLen(buf, pos);
        pos = len.pos;
        if (metaType === 0x03) {
          trackName = '';
          for (let i = 0; i < len.value; i++) trackName += String.fromCharCode(buf[pos + i]);
        } else if (metaType === 0x51 && len.value === 3) {
          tempo = (buf[pos] << 16) | (buf[pos + 1] << 8) | buf[pos + 2];
        }
        pos += len.value;
      } else if (status === 0xf0 || status === 0xf7) {
        const len = readVarLen(buf, pos);
        pos = len.pos + len.value;
      }
    }

    tracks.push({ name: trackName, events });
  }

  return { format, numTracks, division, tracks, tempo };
}

function buildNoteList(track) {
  const notes = [];
  const pending = new Map();

  for (const ev of track.events) {
    if (ev.type === 'on') {
      pending.set(ev.note, ev.tick);
    } else if (ev.type === 'off' && pending.has(ev.note)) {
      const startTick = pending.get(ev.note);
      const duration = ev.tick - startTick;
      const info = midiToNote(ev.note);
      notes.push({ ...info, startTick, duration, midi: ev.note });
      pending.delete(ev.note);
    }
  }

  notes.sort((a, b) => a.startTick - b.startTick || a.midi - b.midi);
  return notes;
}

function notesToBells(notes, division) {
  const eighth = division / 2;
  let bells = '';
  let currentTick = 0;

  for (let i = 0; i < notes.length; i++) {
    const note = notes[i];

    const chord = [note];
    while (i + 1 < notes.length && notes[i + 1].startTick === note.startTick) {
      chord.push(notes[++i]);
    }

    const gap = note.startTick - currentTick;
    const restCount = Math.round(gap / eighth);
    for (let r = 0; r < restCount; r++) bells += '/';

    const maxDur = Math.max(...chord.map(n => n.duration));
    const eighths = Math.max(1, Math.round(maxDur / eighth));
    const tildes = Math.max(0, eighths - 1);

    if (chord.length > 1) {
      bells += '[';
      for (const n of chord) {
        bells += n.str;
      }
      for (let t = 0; t < tildes; t++) bells += '~';
      bells += ']';
    } else {
      bells += note.str;
      for (let t = 0; t < tildes; t++) bells += '~';
    }

    currentTick = note.startTick + maxDur;
  }

  return bells;
}

async function main() {
  const path = process.argv[2];
  if (!path) {
    console.error('Usage: ant midi2bells.js <path-to-midi>');
    process.exit(1);
  }

  const buf = await readFile(path);
  const data = new Uint8Array(buf);
  const midi = parseMidi(data);

  console.log('\n=== Tracks ===');
  for (const track of midi.tracks) {
    const noteEvents = track.events.filter(e => e.type === 'on');
    console.log(`${track.name}: ${noteEvents.length} note-on events`);
  }

  const melodicTracks = midi.tracks.filter(t => {
    const notes = t.events.filter(e => e.type === 'on' && e.channel !== 9);
    return notes.length > 0;
  });

  console.log(`\nMelodic tracks: ${melodicTracks.map(t => t.name).join(', ')}`);

  const bellParts = [];
  for (const track of melodicTracks) {
    const filteredEvents = track.events.filter(e => e.channel !== 9);
    const notes = buildNoteList({ events: filteredEvents }, midi.division);
    if (notes.length === 0) continue;
    const bells = notesToBells(notes, midi.division);
    console.log(`\n--- ${track.name} (${notes.length} notes) ---`);
    console.log(bells.substring(0, 200) + (bells.length > 200 ? '...' : ''));
    bellParts.push({ name: track.name, bells });
  }

  const order = ['square', 'overdrive', 'bass', 'organ', 'brass'];
  const sorted = [];
  for (const name of order) {
    const found = bellParts.find(p => p.name.toLowerCase().includes(name));
    if (found) sorted.push(found);
  }
  for (const p of bellParts) {
    if (!sorted.includes(p)) sorted.push(p);
  }

  const bpm = Math.round(60000000 / midi.tempo);
  const combined = `${bpm}${sorted.map(p => p.bells).join('|')}`;
  console.log('\n\n=== FINAL BELL NOTATION ===\n');
  console.log(combined);
}

main();
