#include "status.h"

#include <string.h>
#include <stdio.h>

#include "packet.h"

static size_t json_escape_string(char *dst, size_t dst_size, const char *src) {
    size_t out = 0;

    while (*src != '\0') {
        const char *replacement = NULL;
        char ch = *src++;

        if (ch == '\\') {
            replacement = "\\\\";
        } else if (ch == '"') {
            replacement = "\\\"";
        } else if (ch == '\n') {
            replacement = "\\n";
        } else if (ch == '\r') {
            replacement = "\\r";
        } else if (ch == '\t') {
            replacement = "\\t";
        }

        if (replacement != NULL) {
            size_t repl_len = strlen(replacement);
            if (out + repl_len >= dst_size) {
                break;
            }
            memcpy(dst + out, replacement, repl_len);
            out += repl_len;
            continue;
        }

        if (out + 1 >= dst_size) {
            break;
        }
        dst[out++] = ch;
    }

    if (dst_size > 0) {
        dst[out < dst_size ? out : (dst_size - 1)] = '\0';
    }
    return out;
}

size_t build_status_response(uint8_t *outbuf,
                             size_t outbuf_size,
                             const char *protocol_name,
                             int protocol_number,
                             int max_players,
                             int online_players,
                             const char *motd) {
    char escaped_motd[512];
    char escaped_protocol_name[128];
    char json[1024];
    int json_written;

    if (protocol_name == NULL) {
        protocol_name = "Vectora 1.20.x";
    }
    if (motd == NULL) {
        motd = "Welcome to Vectora!";
    }

    json_escape_string(escaped_protocol_name, sizeof(escaped_protocol_name), protocol_name);
    json_escape_string(escaped_motd, sizeof(escaped_motd), motd);
    json_written = snprintf(
        json,
        sizeof(json),
        "{"
        "\"version\":{\"name\":\"%s\",\"protocol\":%d},"
        "\"players\":{\"max\":%d,\"online\":%d},"
        "\"description\":{\"text\":\"%s\"}"
        "}",
        escaped_protocol_name,
        protocol_number,
        max_players,
        online_players,
        escaped_motd);
    if (json_written < 0 || (size_t)json_written >= sizeof(json)) {
        return 0;
    }

    size_t json_len = strlen(json);
    uint8_t strbuf[512];
    size_t varint_len = write_varint(strbuf, (int)json_len);
    memcpy(strbuf + varint_len, json, json_len);

    // Build packet: [packet id][response string]
    uint8_t packet[1024];
    size_t offset = 0;
    offset += write_varint(packet + offset, 0x00); // Status Response packet id
    memcpy(packet + offset, strbuf, varint_len + json_len);
    offset += varint_len + json_len;

    // Prepend packet length as VarInt
    size_t outlen = write_varint(outbuf, (int)offset);
    memcpy(outbuf + outlen, packet, offset);
    outlen += offset;
    return outlen;
}
