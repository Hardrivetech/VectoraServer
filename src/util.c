#include "util.h"
#include "protocol.h"
#include <stdlib.h>
#include <string.h>

char *read_mc_string(const uint8_t **buffer, size_t *buflen) {
    int32_t strlen = read_varint(buffer, buflen);
    if (strlen < 0 || (size_t)strlen > *buflen) return NULL;
    char *str = (char*)malloc(strlen + 1);
    if (!str) return NULL;
    memcpy(str, *buffer, strlen);
    str[strlen] = '\0';
    *buffer += strlen;
    *buflen -= strlen;
    return str;
}
