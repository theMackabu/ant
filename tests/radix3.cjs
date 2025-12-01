// Radix3 - True Character-Level Radix Tree Router
// Implements compressed prefix matching with automatic node splitting

class Radix3Node {
  constructor() {
    this.prefix = "";           // Character sequence prefix
    this.handler = undefined;   // Handler at this exact path
    this.children = [];         // Static children nodes
    this.paramChild = undefined;    // :param child
    this.wildcardChild = undefined; // *wildcard child
    this.paramName = undefined;     // Name for param/wildcard
  }
}

class Radix3 {
  constructor() {
    this.root = new Radix3Node();
  }
  
  // Find longest common prefix between two strings
  longestCommonPrefix(a, b) {
    let minLen = a.length < b.length ? a.length : b.length;
    for (let i = 0; i < minLen; i = i + 1) {
      if (a[i] !== b[i]) {
        return i;
      }
    }
    return minLen;
  }
  
  // Insert a route into the radix tree
  insert(path, handler) {
    this.insertPath(this.root, path, handler, 0);
  }
  
  insertPath(node, path, handler, start) {
    // Base case: consumed entire path
    if (start >= path.length) {
      node.handler = handler;
      return;
    }
    
    let char = path[start];
    
    // Handle parameter segments (:param)
    if (char === ":") {
      let end = start + 1;
      for (let i = end; i < path.length; i = i + 1) {
        if (path[i] === "/") {
          break;
        }
        end = i + 1;
      }
      
      let paramName = path.substring(start + 1, end);
      
      if (node.paramChild === undefined) {
        node.paramChild = new Radix3Node();
        node.paramChild.paramName = paramName;
      }
      
      this.insertPath(node.paramChild, path, handler, end);
      return;
    }
    
    // Handle wildcard segments (*wildcard)
    if (char === "*") {
      let paramName = path.substring(start + 1, path.length);
      
      if (node.wildcardChild === undefined) {
        node.wildcardChild = new Radix3Node();
        node.wildcardChild.paramName = paramName;
      }
      
      node.wildcardChild.handler = handler;
      return;
    }
    
    // Handle static paths - character-level matching
    // Find the next special character or end
    let end = start;
    for (let i = start; i < path.length; i = i + 1) {
      if (path[i] === ":" || path[i] === "*") {
        break;
      }
      end = i + 1;
    }
    
    let segment = path.substring(start, end);
    
    // Check existing children for prefix match
    for (let i = 0; i < node.children.length; i = i + 1) {
      let child = node.children[i];
      let commonLen = this.longestCommonPrefix(child.prefix, segment);
      
      if (commonLen > 0) {
        // Found a matching prefix
        
        if (commonLen < child.prefix.length) {
          // Need to split the child node
          // Example: child has "users", we're inserting "user"
          // Split into: parent -> "user" -> "s"
          
          let splitNode = new Radix3Node();
          splitNode.prefix = child.prefix.substring(commonLen, child.prefix.length);
          splitNode.handler = child.handler;
          splitNode.children = child.children;
          splitNode.paramChild = child.paramChild;
          splitNode.wildcardChild = child.wildcardChild;
          
          // Update the current child to hold only the common part
          child.prefix = child.prefix.substring(0, commonLen);
          child.handler = undefined;
          child.children = [splitNode];
          child.paramChild = undefined;
          child.wildcardChild = undefined;
        }
        
        if (commonLen < segment.length) {
          // Continue inserting the remaining path
          this.insertPath(child, path, handler, start + commonLen);
        } else {
          // Exact match of segment - continue with rest of path
          this.insertPath(child, path, handler, end);
        }
        
        return;
      }
    }
    
    // No matching child found - create a new one
    let newChild = new Radix3Node();
    newChild.prefix = segment;
    node.children.push(newChild);
    this.insertPath(newChild, path, handler, end);
  }
  
  // Match a path and return the result
  lookup(path) {
    let params = {};
    let handler = this.matchPath(this.root, path, 0, params);
    
    if (handler !== undefined) {
      return handler(params);
    }
    return undefined;
  }
  
  matchPath(node, path, depth, params) {
    // Base case: consumed entire path
    if (depth >= path.length) {
      return node.handler;
    }
    
    let remaining = path.substring(depth, path.length);
    
    // Try static children first (highest priority)
    for (let i = 0; i < node.children.length; i = i + 1) {
      let child = node.children[i];
      
      // Check if remaining path starts with this child's prefix
      if (remaining.length >= child.prefix.length) {
        let matches = true;
        for (let j = 0; j < child.prefix.length; j = j + 1) {
          if (remaining[j] !== child.prefix[j]) {
            matches = false;
            break;
          }
        }
        
        if (matches) {
          let result = this.matchPath(child, path, depth + child.prefix.length, params);
          if (result !== undefined) {
            return result;
          }
        }
      }
    }
    
    // Try parameter child (medium priority)
    if (node.paramChild !== undefined) {
      // Extract parameter value up to next / or end
      let paramValue = "";
      let offset = depth;
      
      for (let i = depth; i < path.length; i = i + 1) {
        if (path[i] === "/") {
          break;
        }
        paramValue = paramValue + path[i];
        offset = i + 1;
      }
      
      if (paramValue !== "") {
        params[node.paramChild.paramName] = paramValue;
        let result = this.matchPath(node.paramChild, path, offset, params);
        if (result !== undefined) {
          return result;
        }
        // Backtrack
        delete params[node.paramChild.paramName];
      }
    }
    
    // Try wildcard child (lowest priority)
    if (node.wildcardChild !== undefined) {
      let wildcardValue = path.substring(depth, path.length);
      params[node.wildcardChild.paramName] = wildcardValue;
      return node.wildcardChild.handler;
    }
    
    return undefined;
  }
  
  // Debug: Print tree structure
  printTree() {
    this.printNode(this.root, "", true);
  }
  
  printNode(node, prefix, isLast) {
    let marker = isLast ? "└─ " : "├─ ";
    let line = prefix + marker;
    
    if (node.prefix !== "") {
      line = line + "\"" + node.prefix + "\"";
    } else {
      line = line + "(root)";
    }
    
    if (node.handler !== undefined) {
      line = line + " [HANDLER]";
    }
    
    if (node.paramName !== undefined) {
      line = line + " :" + node.paramName;
    }
    
    console.log(line);
    
    let childPrefix = prefix + (isLast ? "    " : "│   ");
    
    // Print static children
    for (let i = 0; i < node.children.length; i = i + 1) {
      let isLastChild = (i === node.children.length - 1) && 
                        (node.paramChild === undefined) && 
                        (node.wildcardChild === undefined);
      this.printNode(node.children[i], childPrefix, isLastChild);
    }
    
    // Print param child
    if (node.paramChild !== undefined) {
      let isLastChild = node.wildcardChild === undefined;
      console.log(childPrefix + (isLastChild ? "└─ " : "├─ ") + ":" + node.paramChild.paramName);
      this.printNode(node.paramChild, childPrefix + (isLastChild ? "    " : "│   "), true);
    }
    
    // Print wildcard child
    if (node.wildcardChild !== undefined) {
      console.log(childPrefix + "└─ *" + node.wildcardChild.paramName);
      this.printNode(node.wildcardChild, childPrefix + "    ", true);
    }
  }
}

// ============================================================================
// Tests
// ============================================================================

console.log("=== Radix3 Router Tests ===\n");

let router = new Radix3();

// Insert routes demonstrating prefix compression
router.insert("/", function(p) { return "Root"; });
router.insert("/user", function(p) { return "User (singular)"; });
router.insert("/users", function(p) { return "Users (plural)"; });
router.insert("/users/list", function(p) { return "Users list"; });
router.insert("/users/:id", function(p) { return "User ID: " + p.id; });
router.insert("/users/:id/posts", function(p) { return "Posts for user: " + p.id; });
router.insert("/search", function(p) { return "Search"; });
router.insert("/static/home", function(p) { return "Static home"; });
router.insert("/static/about", function(p) { return "Static about"; });
router.insert("/api/v1/users", function(p) { return "API v1 users"; });
router.insert("/api/v2/users", function(p) { return "API v2 users"; });
router.insert("/files/*path", function(p) { return "File: " + p.path; });
router.insert("/docs/*rest", function(p) { return "Docs: " + p.rest; });

console.log("Tree structure:");
console.log("===============");
router.printTree();
console.log("");

// Run tests
console.log("Lookup tests:");
console.log("=============");

console.log("1. /");
console.log("   =>", router.lookup("/"));

console.log("\n2. /user");
console.log("   =>", router.lookup("/user"));

console.log("\n3. /users");
console.log("   =>", router.lookup("/users"));

console.log("\n4. /users/list");
console.log("   =>", router.lookup("/users/list"));

console.log("\n5. /users/123");
console.log("   =>", router.lookup("/users/123"));

console.log("\n6. /users/456/posts");
console.log("   =>", router.lookup("/users/456/posts"));

console.log("\n7. /search");
console.log("   =>", router.lookup("/search"));

console.log("\n8. /static/home");
console.log("   =>", router.lookup("/static/home"));

console.log("\n9. /static/about");
console.log("   =>", router.lookup("/static/about"));

console.log("\n10. /api/v1/users");
console.log("    =>", router.lookup("/api/v1/users"));

console.log("\n11. /api/v2/users");
console.log("    =>", router.lookup("/api/v2/users"));

console.log("\n12. /files/images/photo.jpg");
console.log("    =>", router.lookup("/files/images/photo.jpg"));

console.log("\n13. /docs/api/reference/auth");
console.log("    =>", router.lookup("/docs/api/reference/auth"));

console.log("\n14. /notfound (404)");
console.log("    =>", router.lookup("/notfound"));

console.log("\n=== All Tests Complete ===");
