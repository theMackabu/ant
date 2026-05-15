console.log(
  Ant.highlight(`
export default async function demoHighlight(
  root = globalThis?.window?.document?.querySelector("#app")
) {
  // highlight repl demo
  const profile = {
    "name": "Ada",
    "active": "true",
    enabled: true,
    age: 0x2A,
    tags: ["js", "ux"],
  };

  const metrics = {
    bin: 0b1010,
    oct: 0o17,
    dec: 42_000,
    big: 99n,
    ratio: 1.25e3, // cool inline comment
  };

  class Painter extends CanvasEngine {
    #filePath = "/tmp/demo.json";

    #mix(a, b = 0) {
      return (a + b) / 2;
    }

    render({ width, height }, scale = 1) {
      const rx = /^(?<id>[A-Z_][A-Z0-9_]*)\s*=\s*["'\`]?[\w.-]+$/giu;
      const name = profile?.name ?? "unknown";

      console.log(\`hello \${this.#filePath} :: \${width}x\${height} :: \${name}\`);

      if (rx.test("USER_ID=abc-123") && width >= 320 && height <= 1080) {
        this.#mix(width, height);
      }

      return profile.tags.map((tag, i) => \`\${i}: \${tag}\`).join(", ");
    }
  }

  const painter = new Painter();
  delete profile.temp;
  
  const value = (NaN !== Infinity) ? null : undefined;
  const squares = [...Array(4)].map((_, i) => i * i).join(";");

  return painter.render({ 
    width: 640,
    height: 480 
  }) + " | " + value + " | " + metrics.dec + " | " + squares;
}`)
);
