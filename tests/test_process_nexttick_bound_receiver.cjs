async function main() {
  const receiver = {
    tag: 'receiver',
    seen: [],
    callback(arg) {
      this.seen.push({ thisTag: this.tag, arg });
    },
  };

  const bound = receiver.callback.bind(receiver);
  process.nextTick(bound, 'queued');

  await new Promise((resolve) => setTimeout(resolve, 0));

  if (receiver.seen.length !== 1) {
    throw new Error(`expected 1 callback invocation, got ${receiver.seen.length}`);
  }

  const [entry] = receiver.seen;
  if (entry.thisTag !== 'receiver') {
    throw new Error(`expected bound receiver, got ${JSON.stringify(entry.thisTag)}`);
  }

  if (entry.arg !== 'queued') {
    throw new Error(`expected queued arg, got ${JSON.stringify(entry.arg)}`);
  }

  console.log('process.nextTick preserves bound receiver and queued args');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
