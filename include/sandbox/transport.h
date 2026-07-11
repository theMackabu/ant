#ifndef ANT_SANDBOX_TRANSPORT_H
#define ANT_SANDBOX_TRANSPORT_H

#include "sandbox/sandbox.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ANT_SANDBOX_TRANSPORT_VSOCK_HOST_CID 2u
#define ANT_SANDBOX_TRANSPORT_VSOCK_PORT 1024u

bool ant_sandbox_transport_send_exit(int code);
bool ant_sandbox_transport_install_output_frames(void);
int ant_sandbox_transport_fd(void);

bool ant_sandbox_read_request_transport(ant_sandbox_request_t *out);
bool ant_sandbox_transport_send_frame(ant_sandbox_frame_type_t type, const void *payload, size_t payload_len);

#endif
