#ifndef STATUS_H
#define STATUS_H

#include <stdint.h>
#include <stddef.h>

// Handles a status request and writes the response JSON to outbuf, returns length
size_t build_status_response(uint8_t *outbuf, size_t outbuf_size);

#endif // STATUS_H
