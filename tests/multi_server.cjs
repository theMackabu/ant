// Example: Running multiple HTTP servers on different ports

function handler8000(req, res) {
  return {
    status: 200,
    body: "Server on port 8000\nURI: " + req.uri
  };
}

function handler8001(req, res) {
  return {
    status: 200,
    body: "Server on port 8001\nURI: " + req.uri
  };
}

Ant.println("Starting multiple HTTP servers...");

// Note: Currently Ant.serve() is blocking, so you can only run one server
// In the future, we could make it non-blocking to support truly concurrent servers
Ant.println("Starting server on port 8000...");
Ant.serve(8000, handler8000);
