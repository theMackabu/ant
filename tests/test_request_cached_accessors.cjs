const request = new Request("https://example.com/path?query=1");

for (let i = 0; i < 20_000; i++) {
  if (request.method !== "GET") throw new Error("cached Request.method changed");
  if (request.url !== "https://example.com/path?query=1")
    throw new Error("cached Request.url changed");
}

const clone = request.clone();
if (clone.method !== "GET") throw new Error("cloned Request.method mismatch");
if (clone.url !== request.url) throw new Error("cloned Request.url mismatch");

console.log("Request cached accessor tests passed");
