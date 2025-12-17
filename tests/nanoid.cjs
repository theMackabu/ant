let alphabet = 'useandom-26T198340PX75pxJACKVERYMINDBUSHWOLF_GQZbfghjklqvwyzrict';

function nanoid(size) {
  let id = '';
  const randomBytes = crypto.randomBytes(size);

  for (let i = 0; i < size; i++) {
    id += alphabet[63 & randomBytes[i]];
  }

  return id;
}

console.log(nanoid(21));
