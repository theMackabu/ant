// this intentionally consumes CPU continuously. the guest is allowed to burst
// at full vCPU speed and is stopped once its cumulative CPU budget is spent.
let value = 1;

for (;;) {
  value = Math.imul(value ^ 0x9e3779b9, 2654435761);
}
