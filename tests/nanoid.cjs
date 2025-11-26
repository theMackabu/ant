let alphabet = 'useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict';

function nanoid(size) {
  let id = '';
  const randomBytes = Ant.Crypto.randomBytes(size);

  for (let i = 0; i < size; i++) {
    id += alphabet[63 & randomBytes[i]];
  }

  return id;
}

Ant.println(nanoid(21));
