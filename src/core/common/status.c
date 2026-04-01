#include "status.h"
#include <string.h>
#include "packet.h"

size_t build_status_response(uint8_t *outbuf, size_t outbuf_size) {
    // Example response (MOTD, version, player count)
    const char *json =
        "{"
        "\"version\":{\"name\":\"Vectora 1.20.x\",\"protocol\":774},"
        "\"players\":{\"max\":20,\"online\":0},"
        "\"description\":{\"text\":\"Welcome to Vectora!\"}"
        "}";
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
