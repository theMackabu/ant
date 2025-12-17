// Example: Running multiple HTTP servers on different ports

function handler8000(c) {
  c.res.body("Server on port 8000\nURI: " + c.req.uri);
}

function handler8001(c) {
  c.res.body("Server on port 8001\nURI: " + c.req.uri);
}

console.log("Starting multiple HTTP servers...");

// Note: Currently Ant.serve() is blocking, so you can only run one server
// In the future, we could make it non-blocking to support truly concurrent servers
console.log("Starting server on port 8000...");
Ant.serve(8000, handler8000);
