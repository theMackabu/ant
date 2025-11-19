// Example HTTP server with basic routing

function handleRequest(req, res) {
  Ant.println("Request:", req.method, req.uri);
  
  // Simple routing based on URI
  if (req.uri === "/") {
    return {
      status: 200,
      body: "Welcome to Ant HTTP Server!\n\nAvailable routes:\n  GET /\n  GET /hello\n  GET /status\n  GET /echo"
    };
  }
  
  if (req.uri === "/hello") {
    return {
      status: 200,
      body: "Hello, World!"
    };
  }
  
  if (req.uri === "/status") {
    return {
      status: 200,
      body: "Server is running!"
    };
  }
  
  if (req.uri === "/echo") {
    return {
      status: 200,
      body: "Method: " + req.method + "\nURI: " + req.uri + "\nQuery: " + req.query + "\nBody: " + req.body
    };
  }
  
  // 404 for unknown routes
  return {
    status: 404,
    body: "Not Found: " + req.uri
  };
}

Ant.println("Starting HTTP server on port 8000...");
Ant.serve(8000, handleRequest);
