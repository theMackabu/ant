// Example HTTP server using Ant.serve

// Define a request handler
function handleRequest(req, res) {
  Ant.println("Received request:", req.method, req.uri);
  
  // Return a response object
  return {
    status: 200,
    body: "Hello from Ant HTTP Server!\nMethod: " + req.method + "\nURI: " + req.uri
  };
}

// Start the server on port 8000
Ant.println("Starting HTTP server...");
Ant.serve(8000, handleRequest);
