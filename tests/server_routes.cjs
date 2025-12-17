// Example HTTP server with basic routing

function handleRequest(c) {
  console.log("Request:", c.req.method, c.req.uri);
  
  // Simple routing based on URI
  if (c.req.uri === "/") {
    c.res.body("Welcome to Ant HTTP Server!\n\nAvailable routes:\n  GET /\n  GET /hello\n  GET /status\n  GET /echo");
  }
  else if (c.req.uri === "/hello") {
    c.res.body("Hello, World!");
  }
  else if (c.req.uri === "/status") {
    c.res.header('X-Server', 'Ant');
    c.res.body("Server is running!");
  }
  else if (c.req.uri === "/echo") {
    const userAgent = c.req.header('User-Agent') || 'Unknown';
    c.res.body("Method: " + c.req.method + "\nURI: " + c.req.uri + "\nQuery: " + c.req.query + "\nBody: " + c.req.body + "\nUser-Agent: " + userAgent);
  }
  else {
    // 404 for unknown routes
    c.res.status(404);
    c.res.body("Not Found: " + c.req.uri);
  }
}

console.log("Starting HTTP server on port 8000...");
Ant.serve(8000, handleRequest);
