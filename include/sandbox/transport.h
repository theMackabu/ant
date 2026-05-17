#ifndef ANT_SANDBOX_TRANSPORT_H
#define ANT_SANDBOX_TRANSPORT_H

#include "sandbox/sandbox.h"

#include <stdbool.h>
#include <stddef.h>

#define ANT_SANDBOX_TRANSPORT_VSOCK_HOST_CID 2u
#define ANT_SANDBOX_TRANSPORT_VSOCK_PORT 1024u

bool ant_sandbox_read_request_transport(ant_sandbox_request_t *out);

#endif
