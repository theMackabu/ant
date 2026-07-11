let value = 1;

for (;;) {
  value = Math.imul(value ^ 0x9e3779b9, 2654435761);
}
