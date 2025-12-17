// Example HTTP server using Ant.serve

// Define a request handler
function handleRequest(c) {
  console.log('Received request:', c.req.method, c.req.uri);

  const userAgent = c.req.header('User-Agent');
  c.res.header('X-Message', 'My custom message');

  // Return a response
  c.res.body('Hello from Ant HTTP Server!\nMethod: ' + c.req.method + '\nURI: ' + c.req.uri + '\nUser-Agent: ' + userAgent);
}

// Start the server on port 8000
console.log('Starting HTTP server...');
Ant.serve(8000, handleRequest);
