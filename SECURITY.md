# Security Policy

## Supported Versions

| Version  | Supported |
| -------- | --------- |
| latest   | ✅        |
| pre v0.5 | ❌        |

## Reporting a Vulnerability

If you discover a security vulnerability in Ant, please report it responsibly:

1. **Do not** open a public GitHub issue
2. Email security concerns to: **themackabu@gmail.com**
3. Include:
   - Description of the vulnerability
   - Steps to reproduce
   - Potential impact
   - Any suggested fixes (optional)

## Response Timeline

- **Acknowledgment**: Within 12 hours
- **Initial assessment**: Within 2 days
- **Fix timeline**: Depends on severity (critical issues prioritized)

## Security Considerations

Ant is a JavaScript runtime with system-level access. When using Ant:

- **FFI**: The `ant:ffi` module provides direct memory access. Only load trusted native libraries.
- **Shell execution**: The `ant:shell` module executes system commands. Sanitize all user input.
- **URL imports**: Remote module imports execute code from external sources. Only import from trusted origins.
- **File system**: The `ant:fs` module has full filesystem access. Validate paths carefully.

## Disclosure Policy

Once a vulnerability is fixed, we will:

1. Release a patched version
2. Credit the reporter (if desired)
3. Publish a security advisory on GitHub
