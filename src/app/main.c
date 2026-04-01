#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "protocol.h"
#include "packet.h"
#include "configuration_packets.h"
#include "configuration_replay.h"
#include "server_config.h"
#include "status.h"
#include "join_game.h"
#include "player_position.h"
#include "play_packets.h"
#include "world_loader.h"
#include "chunk_sender.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_handle_t;
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
typedef int socket_handle_t;
#endif

#define BACKLOG 5

// Helper to double-frame a packet: [VarInt total length][VarInt uncompressed length][packet data]
// Requires zlib.h
// zlib for compression
#include <zlib.h>
static int compression_threshold = 256; // Match Set Compression threshold
static int g_log_packet_framing = 1;
static int g_log_play_packets = 0;
static int g_log_chunk_sends = 1;

#ifdef _WIN32
static volatile LONG g_connected_play_sessions = 0;
static int load_connected_play_sessions(void) {
    return (int)InterlockedCompareExchange(&g_connected_play_sessions, 0, 0);
}
static void increment_connected_play_sessions(void) {
    InterlockedIncrement(&g_connected_play_sessions);
}
static void decrement_connected_play_sessions(void) {
    InterlockedDecrement(&g_connected_play_sessions);
}
#else
static volatile int g_connected_play_sessions = 0;
static int load_connected_play_sessions(void) {
    return __sync_add_and_fetch(&g_connected_play_sessions, 0);
}
static void increment_connected_play_sessions(void) {
    __sync_add_and_fetch(&g_connected_play_sessions, 1);
}
static void decrement_connected_play_sessions(void) {
    __sync_sub_and_fetch(&g_connected_play_sessions, 1);
}
#endif

typedef struct {
    int32_t stream_center_chunk_x;
    int32_t stream_center_chunk_z;
    int chunk_stream_radius;
    int force_debug_spawn;
    int has_world_info;
} client_stream_state_t;

static void send_large_post_compression_packet(socket_handle_t socket_fd,
                                               const uint8_t *packet,
                                               size_t packet_len);

static uint64_t read_be64_ptr(const uint8_t *src) {
    return ((uint64_t)src[0] << 56) |
           ((uint64_t)src[1] << 48) |
           ((uint64_t)src[2] << 40) |
           ((uint64_t)src[3] << 32) |
           ((uint64_t)src[4] << 24) |
           ((uint64_t)src[5] << 16) |
           ((uint64_t)src[6] << 8) |
            (uint64_t)src[7];
}

static double read_be_double_ptr(const uint8_t *src) {
    union {
        uint64_t bits;
        double value;
    } u;
    u.bits = read_be64_ptr(src);
    return u.value;
}

static int read_post_compression_packet_data(const uint8_t *buf,
                                             size_t buf_len,
                                             int32_t *out_packet_id,
                                             const uint8_t **out_payload,
                                             size_t *out_payload_len,
                                             uint8_t *inflate_buf,
                                             size_t inflate_buf_size) {
    const uint8_t *p = buf;
    size_t len = buf_len;

    if (buf == NULL || out_packet_id == NULL || out_payload == NULL || out_payload_len == NULL) {
        return 0;
    }

    (void)read_varint(&p, &len);
    if (len == 0) {
        return 0;
    }

    {
        int32_t data_len = read_varint(&p, &len);
        if (data_len < 0) {
            return 0;
        }

        if (data_len == 0) {
            const uint8_t *payload_ptr = p;
            size_t payload_len = len;
            *out_packet_id = read_varint(&payload_ptr, &payload_len);
            if ((int)*out_packet_id < 0) {
                return 0;
            }
            *out_payload = payload_ptr;
            *out_payload_len = payload_len;
            return 1;
        }
    }

    {
        uLongf inflated_len = (uLongf)inflate_buf_size;
        int zres = uncompress(inflate_buf, &inflated_len, p, (uLong)len);
        if (zres != Z_OK || inflated_len == 0) {
            return 0;
        }

        {
            const uint8_t *ip = inflate_buf;
            size_t ilen = (size_t)inflated_len;
            *out_packet_id = read_varint(&ip, &ilen);
            if ((int)*out_packet_id < 0) {
                return 0;
            }
            *out_payload = ip;
            *out_payload_len = ilen;
            return 1;
        }
    }
}

static int try_extract_position_from_play_packet(int32_t packet_id,
                                                 const uint8_t *payload,
                                                 size_t payload_len,
                                                 double *out_x,
                                                 double *out_z) {
    /*
     * Accept common serverbound movement packet IDs used across nearby protocol
     * revisions. Position packets always start with x,y,z doubles.
     */
    int is_position_packet =
        (packet_id == 0x1A) || (packet_id == 0x1B) ||
        (packet_id == 0x15) || (packet_id == 0x16);

    if (!is_position_packet || payload == NULL || payload_len < 24 || out_x == NULL || out_z == NULL) {
        return 0;
    }

    *out_x = read_be_double_ptr(payload);
    *out_z = read_be_double_ptr(payload + 16);
    return 1;
}

static int send_stream_chunk(socket_handle_t socket_fd,
                             int32_t chunk_x,
                             int32_t chunk_z,
                             const server_config_t *server_config,
                             int force_debug_spawn,
                             int has_world_info,
                             const world_info_t *world_info) {
    int sent_this = 0;

    if (!force_debug_spawn && server_config->enable_real_chunks && has_world_info) {
        if (world_info->has_spawn_chunk &&
            chunk_x == world_info->spawn_chunk_x &&
            chunk_z == world_info->spawn_chunk_z) {
            size_t real_len = 0;
            uint8_t *real_buf = build_chunk_data_packet(
                world_info->spawn_chunk_nbt,
                world_info->spawn_chunk_nbt_len,
                chunk_x,
                chunk_z,
                &real_len);
            if (real_buf) {
                send_large_post_compression_packet(socket_fd, real_buf, real_len);
                if (g_log_chunk_sends) {
                    printf("Sent REAL chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, real_len);
                }
                free(real_buf);
                sent_this = 1;
            }
        } else {
            uint8_t *chunk_nbt = NULL;
            size_t chunk_nbt_len = 0;
            if (load_chunk_nbt_at(world_info, chunk_x, chunk_z, &chunk_nbt, &chunk_nbt_len)) {
                size_t real_len = 0;
                uint8_t *real_buf = build_chunk_data_packet(
                    chunk_nbt,
                    chunk_nbt_len,
                    chunk_x,
                    chunk_z,
                    &real_len);
                if (real_buf) {
                    send_large_post_compression_packet(socket_fd, real_buf, real_len);
                    if (g_log_chunk_sends) {
                        printf("Sent REAL chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, real_len);
                    }
                    free(real_buf);
                    sent_this = 1;
                }
                free(chunk_nbt);
            }
        }
    }

    if (!sent_this && (force_debug_spawn || !server_config->enable_real_chunks || server_config->allow_debug_chunk_fallback)) {
        size_t dbg_len = 0;
        uint8_t *dbg_buf = build_debug_flat_chunk_packet(chunk_x, chunk_z, &dbg_len);
        if (dbg_buf) {
            send_large_post_compression_packet(socket_fd, dbg_buf, dbg_len);
            if (g_log_chunk_sends) {
                printf("Sent DEBUG flat chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, dbg_len);
            }
            free(dbg_buf);
            sent_this = 1;
        }
    }

    return sent_this;
}

static int resolve_displayed_online_players(const server_config_t *server_config) {
    int displayed_online_players = server_config->online_players_display;

    if (server_config->online_players_mode == ONLINE_PLAYERS_MODE_ZERO) {
        displayed_online_players = 0;
    } else if (server_config->online_players_mode == ONLINE_PLAYERS_MODE_CONNECTED) {
        displayed_online_players = load_connected_play_sessions();
    }

    return displayed_online_players;
}

static int resolve_chunk_stream_radius(const server_config_t *server_config) {
    if (server_config->chunk_stream_radius > 0) {
        return server_config->chunk_stream_radius;
    }

    /* Auto mode derives from view_distance but clamps to a safe range. */
    {
        int derived = server_config->view_distance;
        if (derived < 1) {
            derived = 1;
        }
        if (derived > 8) {
            derived = 8;
        }
        return derived;
    }
}

static int env_flag_enabled(const char *name) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return 0;
    }

    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
        strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0 || strcmp(value, "on") == 0 ||
        strcmp(value, "ON") == 0) {
        return 1;
    }

    return 0;
}

static int load_config_replay_with_fallbacks(config_replay_t *replay, const char **loaded_path) {
    static const char *fallback_paths[] = {
        "replay/config_packets.hex",
        "../replay/config_packets.hex",
        "config_packets.hex",
        "../config_packets.hex"
    };
    char error[256];
    const char *env_path = getenv("VECTORA_CONFIG_REPLAY");

    if (loaded_path != NULL) {
        *loaded_path = NULL;
    }

    if (env_path != NULL && env_path[0] != '\0') {
        if (load_config_replay_from_file(env_path, replay, error, sizeof(error))) {
            if (loaded_path != NULL) {
                *loaded_path = env_path;
            }
            return 1;
        }
        printf("Config replay not loaded from VECTORA_CONFIG_REPLAY: %s\n", error);
    }

    for (size_t i = 0; i < sizeof(fallback_paths) / sizeof(fallback_paths[0]); ++i) {
        if (load_config_replay_from_file(fallback_paths[i], replay, error, sizeof(error))) {
            if (loaded_path != NULL) {
                *loaded_path = fallback_paths[i];
            }
            return 1;
        }
    }

    return 0;
}

// Decode a packet ID from a packet received after Set Compression.
// Handles both uncompressed (Data Length = 0) and compressed payloads.
static int read_post_compression_packet_id(const uint8_t *buf, size_t buf_len, int32_t *out_packet_id) {
    const uint8_t *p = buf;
    size_t len = buf_len;

    if (buf == NULL || out_packet_id == NULL || len == 0) {
        return 0;
    }

    // Outer packet length (not used directly here, but must be consumed).
    (void)read_varint(&p, &len);
    if (len == 0) {
        return 0;
    }

    // Data Length: 0 => uncompressed payload follows, >0 => zlib-compressed payload follows.
    int32_t data_len = read_varint(&p, &len);
    if (data_len < 0) {
        return 0;
    }

    if (data_len == 0) {
        if (len == 0) {
            return 0;
        }
        *out_packet_id = read_varint(&p, &len);
        return 1;
    }

    uint8_t inflated[8192];
    uLongf inflated_len = sizeof(inflated);
    int zres = uncompress(inflated, &inflated_len, p, (uLong)len);
    if (zres != Z_OK) {
        printf("[read_post_compression_packet_id] zlib uncompress error: %d\n", zres);
        return 0;
    }

    const uint8_t *ip = inflated;
    size_t ilen = (size_t)inflated_len;
    if (ilen == 0) {
        return 0;
    }

    *out_packet_id = read_varint(&ip, &ilen);
    return 1;
}

static size_t double_frame_packet(uint8_t *dst, const uint8_t *src, size_t len) {
    size_t off = 0;
    uint8_t inner[4096];
    size_t inner_off = 0;
    int compress = (int)len >= compression_threshold;
    if (compress) {
        // Compress with zlib
        uLongf comp_len = sizeof(inner) - 5; // leave space for VarInt
        int zres = compress2(inner + 5, &comp_len, src, (uLong)len, Z_DEFAULT_COMPRESSION); // Cast len to uLong
        if (zres != Z_OK) {
            printf("[double_frame_packet] zlib compress error: %d\n", zres);
            return 0;
        }
        // When compressed, Data Length is the uncompressed packet size.
        size_t varint_len = write_varint(inner, (int)len);
        memmove(inner + varint_len, inner + 5, comp_len);
        inner_off = varint_len + comp_len;
        // Write total length
        off += write_varint(dst + off, (int)inner_off);
        memcpy(dst + off, inner, inner_off);
        if (g_log_packet_framing) {
            printf("[double_frame_packet] COMPRESSED: orig=%zu, comp=%lu, inner_varint=%d\n", len, comp_len, (int)len);
            printf("[double_frame_packet] first 8 bytes: ");
            for (size_t i = 0; i < 8 && i < comp_len; ++i) printf("%02X ", inner[varint_len + i]);
            printf("\n");
        }
    } else {
        // When not compressed, Data Length must be 0 and payload is raw packet bytes.
        size_t varint_len = write_varint(inner + inner_off, 0);
        memcpy(inner + inner_off + varint_len, src, len);
        if (g_log_packet_framing) {
            printf("[double_frame_packet] UNCOMPRESSED: len=%zu, inner_varint=0\n", len);
            printf("[double_frame_packet] first 8 bytes: ");
            for (size_t i = 0; i < 8 && i < len; ++i) printf("%02X ", src[i]);
            printf("\n");
        }
        inner_off += varint_len + len;
        // Write total length (length of inner frame)
        off += write_varint(dst + off, (int)inner_off);
        memcpy(dst + off, inner, inner_off);
    }
    if (g_log_packet_framing) {
        printf("[double_frame_packet] inner (hex): ");
        for (size_t i = 0; i < inner_off; ++i) printf("%02X ", inner[i]);
        printf("\n[double_frame_packet] outer (hex): ");
        for (size_t i = 0; i < off + inner_off; ++i) printf("%02X ", dst[i]);
        printf("\n");
    }
    return off + inner_off;
}

static int send_all(socket_handle_t socket_fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(socket_fd, (const char *)(buf + sent), (int)(len - sent), 0);
#else
        ssize_t n = send(socket_fd, buf + sent, len - sent, 0);
#endif
        if (n <= 0) {
            return 0;
        }
        sent += (size_t)n;
    }
    return 1;
}

static void send_post_compression_packet(socket_handle_t socket_fd, const uint8_t *packet, size_t packet_len) {
    uint8_t framed[8192];
    size_t framed_len = double_frame_packet(framed, packet, packet_len);

    if (framed_len == 0) {
        return;
    }

    (void)send_all(socket_fd, framed, framed_len);
}

// Large-packet version that uses heap allocation for packets that don't fit in
// the fixed-size framed[] stack buffer.
static void send_large_post_compression_packet(socket_handle_t socket_fd,
                                                const uint8_t *packet,
                                                size_t packet_len) {
    uint8_t outer_vi[5], inner_vi[5];
    size_t  outer_vi_len, inner_vi_len;
    uint8_t *frame = NULL;
    size_t   frame_len;

    if ((int)packet_len >= compression_threshold) {
        uLongf comp_bound = compressBound((uLong)packet_len);
        uint8_t *comp = (uint8_t *)malloc(comp_bound);
        if (!comp) return;
        if (compress2(comp, &comp_bound, packet, (uLong)packet_len,
                      Z_DEFAULT_COMPRESSION) != Z_OK) {
            free(comp);
            return;
        }
        inner_vi_len = write_varint(inner_vi, (int32_t)packet_len);
        size_t inner_len = inner_vi_len + comp_bound;
        outer_vi_len = write_varint(outer_vi, (int32_t)inner_len);
        frame_len = outer_vi_len + inner_len;
        frame = (uint8_t *)malloc(frame_len);
        if (!frame) { free(comp); return; }
        memcpy(frame, outer_vi, outer_vi_len);
        memcpy(frame + outer_vi_len, inner_vi, inner_vi_len);
        memcpy(frame + outer_vi_len + inner_vi_len, comp, comp_bound);
        free(comp);
    } else {
        outer_vi_len = write_varint(outer_vi, (int32_t)(1 + packet_len));
        frame_len = outer_vi_len + 1 + packet_len;
        frame = (uint8_t *)malloc(frame_len);
        if (!frame) return;
        memcpy(frame, outer_vi, outer_vi_len);
        frame[outer_vi_len] = 0x00;
        memcpy(frame + outer_vi_len + 1, packet, packet_len);
    }

    (void)send_all(socket_fd, frame, frame_len);
    free(frame);
}

static void try_serve_one_pending_status(socket_handle_t server_fd, const server_config_t *server_config) {
#ifdef _WIN32
    SOCKET pending_socket = accept(server_fd, NULL, NULL);
    if (pending_socket == INVALID_SOCKET) {
        return;
    }
#else
    int pending_socket = accept(server_fd, NULL, NULL);
    if (pending_socket < 0) {
        return;
    }
#endif

    {
        uint8_t buffer[1024];
#ifdef _WIN32
        int bytes_read = recv(pending_socket, (char*)buffer, sizeof(buffer), 0);
#else
        ssize_t bytes_read = recv(pending_socket, buffer, sizeof(buffer), 0);
#endif
        if (bytes_read > 0) {
            const uint8_t *ptr = buffer;
            size_t buflen = (size_t)bytes_read;
            int32_t packet_length = read_varint(&ptr, &buflen);
            int32_t packet_id = read_varint(&ptr, &buflen);
            int32_t next_state = -1;
            (void)packet_length;

            if (packet_id == 0x00) {
                int32_t protocol_version = read_varint(&ptr, &buflen);
                extern char *read_mc_string(const uint8_t **, size_t *);
                char *server_address = read_mc_string(&ptr, &buflen);
                uint16_t server_port = (uint16_t)(ptr[0] << 8 | ptr[1]);
                ptr += 2;
                buflen -= 2;
                next_state = read_varint(&ptr, &buflen);
                (void)protocol_version;
                (void)server_port;
                free(server_address);
            }

            if (next_state == 1) {
                uint8_t status_buf[1024];
                size_t status_bytes = 0;

                if (buflen > 0) {
                    if (buflen > sizeof(status_buf)) {
                        buflen = sizeof(status_buf);
                    }
                    memcpy(status_buf, ptr, buflen);
                    status_bytes = buflen;
                } else {
#ifdef _WIN32
                    int recv_bytes = recv(pending_socket, (char*)status_buf, sizeof(status_buf), 0);
#else
                    ssize_t recv_bytes = recv(pending_socket, status_buf, sizeof(status_buf), 0);
#endif
                    if (recv_bytes > 0) {
                        status_bytes = (size_t)recv_bytes;
                    }
                }

                if (status_bytes > 0) {
                    const uint8_t *sptr = status_buf;
                    size_t slen = status_bytes;
                    int32_t splen = read_varint(&sptr, &slen);
                    int32_t spid = read_varint(&sptr, &slen);
                    (void)splen;

                    if (spid == 0x00) {
                        int displayed_online_players = resolve_displayed_online_players(server_config);
                        uint8_t outbuf[1024];
                        size_t outlen = build_status_response(
                            outbuf,
                            sizeof(outbuf),
                            server_config->protocol_name,
                            server_config->protocol_number,
                            server_config->max_players,
                            displayed_online_players,
                            server_config->motd);
#ifdef _WIN32
                        send(pending_socket, (const char*)outbuf, (int)outlen, 0);
#else
                        send(pending_socket, outbuf, outlen, 0);
#endif
                    }

#ifdef _WIN32
                    int ping_bytes = recv(pending_socket, (char*)status_buf, sizeof(status_buf), 0);
#else
                    ssize_t ping_bytes = recv(pending_socket, status_buf, sizeof(status_buf), 0);
#endif
                    if (ping_bytes > 0) {
#ifdef _WIN32
                        send(pending_socket, (const char*)status_buf, ping_bytes, 0);
#else
                        send(pending_socket, status_buf, ping_bytes, 0);
#endif
                    }
                }
            }
        }
    }

#ifdef _WIN32
    closesocket(pending_socket);
#else
    close(pending_socket);
#endif
}

int main() {
    server_config_t server_config;
    char server_config_error[256];
    const char *server_config_path = NULL;
    int server_config_status;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
    }
#endif

    server_config_status = load_server_config_with_fallbacks(
        &server_config,
        &server_config_path,
        server_config_error,
        sizeof(server_config_error));
    if (server_config_status < 0) {
        fprintf(stderr, "Failed to load server config: %s\n", server_config_error);
#ifdef _WIN32
        WSACleanup();
#endif
        exit(EXIT_FAILURE);
    }

    compression_threshold = server_config.compression_threshold;
    g_log_packet_framing = server_config.log_packet_framing;
    g_log_play_packets = server_config.log_play_packets;
    g_log_chunk_sends = server_config.log_chunk_sends;

    if (server_config_status > 0) {
        printf("Loaded server config from %s\n", server_config_path);
    } else {
        printf("Server config not found, using built-in defaults.\n");
    }

    // Use SOCKET type for Windows, int for others
#ifdef _WIN32
    SOCKET server_fd;
    SOCKET new_socket;
#else
    int server_fd;
    int new_socket;
#endif
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
#ifdef _WIN32
        WSACleanup();
#endif
        exit(EXIT_FAILURE);
    }

    // Attach socket to the configured port.
#ifdef _WIN32
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
#else
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
#endif
        perror("setsockopt");
#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#endif
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)server_config.port);

    // Bind the socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#endif
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#endif
        exit(EXIT_FAILURE);
    }
    printf("Vectora server listening on port %d...\n", server_config.port);

    while (1) {
        new_socket = (int)accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) {
                // Set TCP_NODELAY to disable Nagle's algorithm (immediate send)
            #ifdef _WIN32
                {
                    BOOL flag = 1;
                    setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
                }
            #else
                {
                    int flag = 1;
                    setsockopt(new_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                }
            #endif
            perror("accept");
#ifdef _WIN32
            closesocket(server_fd);
            WSACleanup();
#endif
            exit(EXIT_FAILURE);
        }
        printf("New connection accepted!\n");

        // Read handshake packet (blocking, simple version)
        uint8_t buffer[1024];
#ifdef _WIN32
        int bytes_read = recv(new_socket, (char*)buffer, sizeof(buffer), 0);
#else
        ssize_t bytes_read = recv(new_socket, buffer, sizeof(buffer), 0);
#endif
        if (bytes_read <= 0) {
#ifdef _WIN32
            closesocket(new_socket);
#else
            close(new_socket);
#endif
            continue;
        }

        // Parse packet length (VarInt)
        const uint8_t *ptr = buffer;
        size_t buflen = bytes_read;
        int32_t packet_length = read_varint(&ptr, &buflen);
        int32_t packet_id = read_varint(&ptr, &buflen);
        printf("Received packet: length=%d, id=%d\n", packet_length, packet_id);

        // Handshake (packet_id 0x00) parsing
        int32_t next_state = -1;
        if (packet_id == 0x00) {
            int32_t protocol_version = read_varint(&ptr, &buflen);
            extern char *read_mc_string(const uint8_t **, size_t *);
            char *server_address = read_mc_string(&ptr, &buflen);
            uint16_t server_port = (uint16_t)(ptr[0] << 8 | ptr[1]);
            ptr += 2; buflen -= 2;
            next_state = read_varint(&ptr, &buflen);
            printf("Handshake: proto=%d, addr=%s, port=%u, next_state=%d\n", protocol_version, server_address, server_port, next_state);
            free(server_address);
        }

        // If next_state is 1 (status), handle status request/response
        if (next_state == 1) {
            // Wait for Status Request packet (id 0x00)
            uint8_t status_buf[1024];
#ifdef _WIN32
            int status_bytes = recv(new_socket, (char*)status_buf, sizeof(status_buf), 0);
#else
            ssize_t status_bytes = recv(new_socket, status_buf, sizeof(status_buf), 0);
#endif
            if (status_bytes > 0) {
                const uint8_t *sptr = status_buf;
                size_t slen = status_bytes;
                int32_t splen = read_varint(&sptr, &slen);
                int32_t spid = read_varint(&sptr, &slen);
                if (spid == 0x00) {
                    int displayed_online_players = resolve_displayed_online_players(&server_config);

                    // Send Status Response
                    uint8_t outbuf[1024];
                    size_t outlen = build_status_response(
                        outbuf,
                        sizeof(outbuf),
                        server_config.protocol_name,
                        server_config.protocol_number,
                        server_config.max_players,
                        displayed_online_players,
                        server_config.motd);
#ifdef _WIN32
                    send(new_socket, (const char*)outbuf, (int)outlen, 0);
#else
                    send(new_socket, outbuf, outlen, 0);
#endif
                    printf("Sent status response to client.\n");
                }
                // Wait for Ping packet (id 0x01)
#ifdef _WIN32
                int ping_bytes = recv(new_socket, (char*)status_buf, sizeof(status_buf), 0);
#else
                ssize_t ping_bytes = recv(new_socket, status_buf, sizeof(status_buf), 0);
#endif
                if (ping_bytes > 0) {
                    // Echo back the ping packet
#ifdef _WIN32
                    send(new_socket, (const char*)status_buf, ping_bytes, 0);
#else
                    send(new_socket, status_buf, ping_bytes, 0);
#endif
                    printf("Echoed ping packet to client.\n");
                }
            }
#ifdef _WIN32
            closesocket(new_socket);
#else
            close(new_socket);
#endif
        } else if (next_state == 2) {
            // Wait for Login Start packet (id 0x00)
            uint8_t login_buf[1024];
#ifdef _WIN32
            int login_bytes = recv(new_socket, (char*)login_buf, sizeof(login_buf), 0);
#else
            ssize_t login_bytes = recv(new_socket, login_buf, sizeof(login_buf), 0);
#endif
            if (login_bytes > 0) {
                const uint8_t *lptr = login_buf;
                size_t llen = login_bytes;
                int32_t lplen = read_varint(&lptr, &llen);
                int32_t lpid = read_varint(&lptr, &llen);
                if (lpid == 0x00) {
                    // Parse username (MC String)
                    extern char *read_mc_string(const uint8_t **, size_t *);
                    char *username = read_mc_string(&lptr, &llen);
                    printf("Login Start: username=%s\n", username);


                    // Send Set Compression packet (id 0x03, Login state)
                    printf("Preparing to send Set Compression...\n");
                    extern size_t build_set_compression_packet(uint8_t *, size_t, int);
                    uint8_t comp_buf[16];
                    size_t comp_buf_len = build_set_compression_packet(
                        comp_buf,
                        sizeof(comp_buf),
                        server_config.compression_threshold);
                    printf("Set Compression packet (hex): ");
                    for (size_t i = 0; i < comp_buf_len; ++i) printf("%02X ", comp_buf[i]);
                    printf("\n");

                    // Send Set Compression with a single frame (not double-framed)
#ifdef _WIN32
                    send(new_socket, (const char*)comp_buf, (int)comp_buf_len, 0);
#else
                    send(new_socket, comp_buf, comp_buf_len, 0);
#endif
                    printf("Sent Set Compression to client.\n");

                    // Add a short delay to ensure client processes Set Compression before Login Success
#ifdef _WIN32
                    Sleep(50); // milliseconds
#else
                    usleep(50000); // microseconds
#endif

                    // Send Login Success packet (id 0x02)
                    printf("Preparing to send Login Success...\n");
                    // login_finished (id 0x02): Profile = UUID(16) + username + properties[]
                    uint8_t uuid[16];
                    extern void write_dummy_uuid(uint8_t *);
                    write_dummy_uuid(uuid);
                    printf("Login Success UUID bytes: ");
                    for (size_t i = 0; i < sizeof(uuid); ++i) printf("%02X ", uuid[i]);
                    printf("\n");

                    // Encode username as MC String
                    uint8_t unamebuf[64];
                    extern size_t write_varint(uint8_t *, int32_t);
                    size_t uname_len = strlen(username);
                    size_t uname_varint = write_varint(unamebuf, (int32_t)uname_len);
                    memcpy(unamebuf + uname_varint, username, uname_len);

                    // Print username encoding for debug
                    printf("Login Success username (len=%zu): ", uname_len);
                    for (size_t i = 0; i < uname_varint + uname_len; ++i) printf("%02X ", unamebuf[i]);
                    printf("\n");


                    // Build Login Success packet (raw, no length prefix)
                    uint8_t packet[128];
                    size_t offset = 0;
                    offset += write_varint(packet + offset, 0x02); // Login Success packet id
                    memcpy(packet + offset, uuid, sizeof(uuid));
                    offset += sizeof(uuid);
                    memcpy(packet + offset, unamebuf, uname_varint + uname_len);
                    offset += uname_varint + uname_len;
                    // Add properties (VarInt 0 for empty array)
                    offset += write_varint(packet + offset, 0);

                    // Print raw Login Success packet (no length prefix)
                    printf("Raw Login Success packet (hex): ");
                    for (size_t i = 0; i < offset; ++i) printf("%02X ", packet[i]);
                    printf("\n");

                    // Login Success double-framing (pass only raw packet, no length prefix)
                    uint8_t double_framed[512];
                    size_t double_framed_len = double_frame_packet(double_framed, packet, offset);
                    printf("Double Framed Login Success packet (hex): ");
                    for (size_t i = 0; i < double_framed_len; ++i) printf("%02X ", double_framed[i]);
                    printf("\n");
                    // Send Login Success
#ifdef _WIN32
                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                    printf("Sent Login Success to client.\n");

                    // Wait for Login Acknowledged (0x03) from client
                    uint8_t ack_buf[256];
#ifdef _WIN32
                    int ack_bytes = recv(new_socket, (char*)ack_buf, sizeof(ack_buf), 0);
#else
                    ssize_t ack_bytes = recv(new_socket, ack_buf, sizeof(ack_buf), 0);
#endif
                    if (ack_bytes > 0) {
                        printf("Received packet after Login Success: ");
                        for (int i = 0; i < ack_bytes; ++i) printf("%02X ", ack_buf[i]);
                        printf("\n");
                        int32_t ackpid = -1;
                        if (!read_post_compression_packet_id(ack_buf, (size_t)ack_bytes, &ackpid)) {
                            printf("Failed to parse packet after Login Success (post-compression format).\n");
                            free(username);
#ifdef _WIN32
                            closesocket(new_socket);
#else
                            close(new_socket);
#endif
                            continue;
                        }
                        printf("Parsed packet ID: %d\n", ackpid);
                        if (ackpid == 0x03) {
                            // Send minimal 1.21 configuration packets
                            extern size_t build_known_packs_packet(uint8_t *, size_t);
                            extern size_t build_feature_flags_packet(uint8_t *, size_t);
                            extern size_t build_finish_config_packet(uint8_t *, size_t);
                            extern size_t build_join_game_packet(uint8_t *, size_t);
                            extern size_t build_player_pos_packet(uint8_t *, size_t);
                            config_replay_t replay;
                            const char *replay_path = NULL;
                            int have_replay;
                            uint8_t cfg_buf[1024]; // 1024 to accommodate damage_type (25 entries ~548 bytes)
                            size_t cfg_len;
                            uint8_t double_framed[2048];
                            size_t double_framed_len;

                            // Known Packs (required for proper registry bootstrap in 1.21+)
                            cfg_len = build_known_packs_packet(cfg_buf, sizeof(cfg_buf));
                            double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                            send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                            send(new_socket, double_framed, double_framed_len, 0);
#endif
                            printf("Sent Known Packs to client.\n");

                            // Wait for Serverbound Known Packs (id 0x07).
                            int got_known_packs = 0;
                            for (;;) {
                                uint8_t kp_buf[1024];
#ifdef _WIN32
                                int kp_bytes = recv(new_socket, (char*)kp_buf, sizeof(kp_buf), 0);
#else
                                ssize_t kp_bytes = recv(new_socket, kp_buf, sizeof(kp_buf), 0);
#endif
                                if (kp_bytes <= 0) {
                                    break;
                                }

                                int32_t kp_pid = -1;
                                if (!read_post_compression_packet_id(kp_buf, (size_t)kp_bytes, &kp_pid)) {
                                    printf("Failed to parse config packet during known-packs negotiation.\n");
                                    continue;
                                }
                                printf("Parsed config packet ID: %d\n", kp_pid);
                                if (kp_pid == 0x07) {
                                    got_known_packs = 1;
                                    break;
                                }
                            }

                            if (!got_known_packs) {
                                printf("Did not receive Serverbound Known Packs (0x07).\n");
                                free(username);
#ifdef _WIN32
                                closesocket(new_socket);
#else
                                close(new_socket);
#endif
                                continue;
                            }

                            have_replay = load_config_replay_with_fallbacks(&replay, &replay_path);
                            if (have_replay) {
                                int replay_sent_finish = 0;
                                printf("Loaded config replay with %zu packets from %s\n", replay.count, replay_path);
                                for (size_t i = 0; i < replay.count; ++i) {
                                    int32_t replay_pid;
                                    const uint8_t *pptr = replay.packets[i];
                                    size_t plen = replay.lengths[i];

                                    replay_pid = read_varint(&pptr, &plen);
                                    double_framed_len = double_frame_packet(double_framed, replay.packets[i], replay.lengths[i]);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent replay config packet id=%d len=%zu\n", replay_pid, replay.lengths[i]);
                                    if (replay_pid == 0x03) {
                                        replay_sent_finish = 1;
                                    }
                                }

                                if (!replay_sent_finish) {
                                    cfg_len = build_finish_config_packet(cfg_buf, sizeof(cfg_buf));
                                    double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Finish Configuration to client.\n");
                                }
                            } else {
                                // Minimal fallback when no replay file is present.
                                cfg_len = build_feature_flags_packet(cfg_buf, sizeof(cfg_buf));
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Feature Flags to client.\n");

                                cfg_len = build_registry_data_packet(cfg_buf, sizeof(cfg_buf));
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (dimension_type) to client.\n");

                                cfg_len = build_registry_data_biome_packet(cfg_buf, sizeof(cfg_buf));
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (biome) to client.\n");

                                cfg_len = build_registry_data_damage_type(cfg_buf, sizeof(cfg_buf));
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (damage_type) to client.\n");

                                // Required non-empty dynamic registries added in 1.21.5+
                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:cat_variant", "minecraft:tabby");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (cat_variant) to client.\n");

                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:chicken_variant", "minecraft:temperate");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (chicken_variant) to client.\n");

                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:cow_variant", "minecraft:temperate");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (cow_variant) to client.\n");

                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:frog_variant", "minecraft:temperate");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (frog_variant) to client.\n");

                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:painting_variant", "minecraft:kebab");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (painting_variant) to client.\n");

                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:pig_variant", "minecraft:temperate");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (pig_variant) to client.\n");

                                cfg_len = build_registry_data_inline_empty(cfg_buf, sizeof(cfg_buf), "minecraft:timeline", "minecraft:overworld");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (timeline) to client.\n");

                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:wolf_sound_variant", "minecraft:classic");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (wolf_sound_variant) to client.\n");

                                cfg_len = build_registry_data_one(cfg_buf, sizeof(cfg_buf), "minecraft:wolf_variant", "minecraft:pale");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (wolf_variant) to client.\n");

                                cfg_len = build_registry_data_with_asset_id(cfg_buf, sizeof(cfg_buf), "minecraft:zombie_nautilus_variant", "minecraft:temperate", "minecraft:zombie_nautilus");
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Registry Data (zombie_nautilus_variant) to client.\n");

                                // Update Tags: bind minecraft:timeline#minecraft:in_overworld to entry 0
                                cfg_len = build_update_tags_with_timeline(cfg_buf, sizeof(cfg_buf));
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Update Tags to client.\n");

                                cfg_len = build_finish_config_packet(cfg_buf, sizeof(cfg_buf));
                                double_framed_len = double_frame_packet(double_framed, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Finish Configuration to client.\n");
                            }

                            // Wait for Acknowledge Finish Configuration (serverbound id 0x03).
                            // The client may send other configuration packets first (e.g. plugin/client info).
                            int got_finish_ack = 0;
                            for (;;) {
                                uint8_t cfg_ack_buf[512];
#ifdef _WIN32
                                int cfg_ack_bytes = recv(new_socket, (char*)cfg_ack_buf, sizeof(cfg_ack_buf), 0);
#else
                                ssize_t cfg_ack_bytes = recv(new_socket, cfg_ack_buf, sizeof(cfg_ack_buf), 0);
#endif
                                if (cfg_ack_bytes <= 0) {
                                    break;
                                }

                                int32_t cfg_pid = -1;
                                if (!read_post_compression_packet_id(cfg_ack_buf, (size_t)cfg_ack_bytes, &cfg_pid)) {
                                    printf("Failed to parse config packet (post-compression format).\n");
                                    continue;
                                }

                                printf("Parsed config packet ID: %d\n", cfg_pid);
                                if (cfg_pid == 0x03) {
                                    got_finish_ack = 1;
                                    break;
                                }
                            }

                            if (!got_finish_ack) {
                                free_config_replay(&replay);
                                printf("Did not receive Acknowledge Finish Configuration (0x03).\n");
                                free(username);
#ifdef _WIN32
                                closesocket(new_socket);
#else
                                close(new_socket);
#endif
                                continue;
                            }

                            free_config_replay(&replay);

                            // Load world metadata for play bootstrap if a world folder is available.
                            world_info_t world_info;
                            char world_error[256];
                            int has_world_info = load_world_info(
                                &world_info,
                                server_config.world_path,
                                world_error,
                                sizeof(world_error));

                            if (has_world_info) {
                                printf("Loaded world '%s' spawn=(%d,%d,%d) chunk=(%d,%d) chunk_nbt=%s (%zu bytes)\n",
                                    world_info.world_path,
                                    world_info.spawn_x,
                                    world_info.spawn_y,
                                    world_info.spawn_z,
                                    world_info.spawn_chunk_x,
                                    world_info.spawn_chunk_z,
                                    world_info.has_spawn_chunk ? "yes" : "no",
                                    world_info.spawn_chunk_nbt_len);
                            } else {
                                printf("World data not loaded: %s\n", world_error);
                            }

                            int force_debug_spawn = server_config.force_debug_spawn || env_flag_enabled("VECTORA_FORCE_DEBUG_SPAWN");
                            if (force_debug_spawn) {
                                printf("VECTORA_FORCE_DEBUG_SPAWN enabled: forcing debug chunks and debug spawn.\n");
                            }

                            // Join Game
                            uint8_t jg_buf[1024];
                            join_game_params_t join_params;
                            memset(&join_params, 0, sizeof(join_params));
                            join_params.entity_id = 1;
                            join_params.dimension_name = has_world_info ? world_info.dimension_name : "minecraft:overworld";
                            join_params.max_players = server_config.max_players;
                            join_params.view_distance = server_config.view_distance;
                            join_params.simulation_distance = server_config.simulation_distance;
                            join_params.previous_game_mode = -1;
                            join_params.sea_level = has_world_info ? world_info.sea_level : 63;
                            join_params.is_flat = has_world_info ? world_info.is_flat : 0;
                            size_t jg_len = build_join_game_packet_ex(jg_buf, sizeof(jg_buf), &join_params);
                            send_post_compression_packet(new_socket, jg_buf, jg_len);
                            printf("Sent Join Game to client.\n");

                            if (server_config.send_brand_packet) {
                                uint8_t brand_buf[256];
                                size_t brand_len = build_brand_packet(brand_buf, sizeof(brand_buf), server_config.server_brand);
                                send_post_compression_packet(new_socket, brand_buf, brand_len);
                                printf("Sent server brand: %s.\n", server_config.server_brand);
                            }

                            // Game Event type 13: "Start waiting for level chunks"
                            // Required since 1.20.3 — without it the client never leaves "Loading terrain"
                            if (server_config.send_wait_for_level_chunks_event) {
                                uint8_t ge_buf[32];
                                size_t ge_len = build_game_event_packet(ge_buf, sizeof(ge_buf), 13, 0.0f);
                                send_post_compression_packet(new_socket, ge_buf, ge_len);
                                printf("Sent Game Event 13 (Start waiting for level chunks).\n");
                            }

                            int32_t debug_chunk_x = has_world_info ? world_info.spawn_chunk_x : 0;
                            int32_t debug_chunk_z = has_world_info ? world_info.spawn_chunk_z : 0;
                            int32_t debug_spawn_x = debug_chunk_x * 16 + 8;
                            int32_t debug_spawn_z = debug_chunk_z * 16 + 8;
                            int32_t debug_spawn_y = 82;
                            int32_t player_spawn_x = debug_spawn_x;
                            int32_t player_spawn_y = debug_spawn_y;
                            int32_t player_spawn_z = debug_spawn_z;
                            int center_chunk_real = 0;
                            client_stream_state_t stream_state;
                            stream_state.chunk_stream_radius = resolve_chunk_stream_radius(&server_config);
                            stream_state.stream_center_chunk_x = debug_chunk_x;
                            stream_state.stream_center_chunk_z = debug_chunk_z;
                            stream_state.force_debug_spawn = force_debug_spawn;
                            stream_state.has_world_info = has_world_info;

                            // Always send Set Center Chunk so the client knows where to load chunks.
                            {
                                int32_t cx = debug_chunk_x;
                                int32_t cz = debug_chunk_z;
                                uint8_t center_buf[64];
                                size_t center_len = build_set_center_chunk_packet(center_buf, sizeof(center_buf), cx, cz);
                                send_post_compression_packet(new_socket, center_buf, center_len);
                                printf("Sent Set Center Chunk for (%d,%d).\n", cx, cz);
                            }

                            // Send an initial chunk area around center so nearby terrain renders reliably.
                            {
                                int32_t chunk_x = debug_chunk_x;
                                int32_t chunk_z = debug_chunk_z;

                                // ChunkBatchStart (0x0C) — no fields
                                uint8_t cbs_buf[4];
                                size_t cbs_len = 0;
                                cbs_len += write_varint(cbs_buf + cbs_len, 0x0C);
                                send_post_compression_packet(new_socket, cbs_buf, cbs_len);

                                (void)has_world_info;

                                int sent_chunks = 0;

                                for (int dz = -stream_state.chunk_stream_radius; dz <= stream_state.chunk_stream_radius; dz++) {
                                    for (int dx = -stream_state.chunk_stream_radius; dx <= stream_state.chunk_stream_radius; dx++) {
                                        int32_t sx = chunk_x + dx;
                                        int32_t sz = chunk_z + dz;
                                        int sent_this = 0;

                                        if (!stream_state.force_debug_spawn && server_config.enable_real_chunks && stream_state.has_world_info && world_info.has_spawn_chunk &&
                                            sx == world_info.spawn_chunk_x && sz == world_info.spawn_chunk_z) {
                                            size_t real_len = 0;
                                            uint8_t *real_buf = build_chunk_data_packet(
                                                world_info.spawn_chunk_nbt,
                                                world_info.spawn_chunk_nbt_len,
                                                sx,
                                                sz,
                                                &real_len);
                                            if (real_buf) {
                                                send_large_post_compression_packet(new_socket, real_buf, real_len);
                                                sent_chunks++;
                                                sent_this = 1;
                                                center_chunk_real = 1;
                                                if (g_log_chunk_sends) {
                                                    printf("Sent REAL chunk for (%d,%d), %zu bytes.\n", sx, sz, real_len);
                                                }
                                                free(real_buf);
                                            } else {
                                                printf("WARNING: failed to build REAL chunk packet (%d,%d), falling back to DEBUG.\n", sx, sz);
                                            }
                                        } else if (!stream_state.force_debug_spawn && server_config.enable_real_chunks && stream_state.has_world_info) {
                                            uint8_t *chunk_nbt = NULL;
                                            size_t chunk_nbt_len = 0;
                                            if (load_chunk_nbt_at(&world_info, sx, sz, &chunk_nbt, &chunk_nbt_len)) {
                                                size_t real_len = 0;
                                                uint8_t *real_buf = build_chunk_data_packet(
                                                    chunk_nbt,
                                                    chunk_nbt_len,
                                                    sx,
                                                    sz,
                                                    &real_len);
                                                if (real_buf) {
                                                    send_large_post_compression_packet(new_socket, real_buf, real_len);
                                                    sent_chunks++;
                                                    sent_this = 1;
                                                    if (g_log_chunk_sends) {
                                                        printf("Sent REAL chunk for (%d,%d), %zu bytes.\n", sx, sz, real_len);
                                                    }
                                                    free(real_buf);
                                                } else {
                                                    printf("WARNING: failed to build REAL chunk packet (%d,%d), falling back to DEBUG.\n", sx, sz);
                                                }
                                                free(chunk_nbt);
                                            }
                                        }

                                        if (!sent_this && (stream_state.force_debug_spawn || !server_config.enable_real_chunks || server_config.allow_debug_chunk_fallback)) {
                                            size_t dbg_len = 0;
                                            uint8_t *dbg_buf = build_debug_flat_chunk_packet(sx, sz, &dbg_len);
                                            if (dbg_buf) {
                                                send_large_post_compression_packet(new_socket, dbg_buf, dbg_len);
                                                sent_chunks++;
                                                if (g_log_chunk_sends) {
                                                    printf("Sent DEBUG flat chunk for (%d,%d), %zu bytes.\n", sx, sz, dbg_len);
                                                }
                                                free(dbg_buf);
                                            } else {
                                                printf("WARNING: failed to build DEBUG flat chunk packet (%d,%d).\n", sx, sz);
                                            }
                                        } else if (!sent_this) {
                                            printf("WARNING: no chunk sent for (%d,%d) because debug fallback is disabled.\n", sx, sz);
                                        }
                                    }
                                }

                                // ChunkBatchFinished (0x0B)
                                uint8_t cbf_buf[8];
                                size_t cbf_len = 0;
                                cbf_len += write_varint(cbf_buf + cbf_len, 0x0B);
                                cbf_len += write_varint(cbf_buf + cbf_len, sent_chunks);
                                send_post_compression_packet(new_socket, cbf_buf, cbf_len);
                                printf("Sent ChunkBatchFinished (batch size=%d).\n", sent_chunks);

                                if (!force_debug_spawn && server_config.enable_real_chunks && center_chunk_real && has_world_info) {
                                    int32_t safe_spawn_y = 0;
                                    int have_safe_spawn = compute_safe_spawn_y_from_chunk_nbt(
                                        world_info.spawn_chunk_nbt,
                                        world_info.spawn_chunk_nbt_len,
                                        world_info.spawn_x,
                                        world_info.spawn_z,
                                        &safe_spawn_y);

                                    player_spawn_x = world_info.spawn_x;
                                    if (have_safe_spawn) {
                                        player_spawn_y = (safe_spawn_y < 319) ? (safe_spawn_y + 1) : safe_spawn_y;
                                    } else {
                                        player_spawn_y = (world_info.spawn_y + 16 > 200) ? (world_info.spawn_y + 16) : 200;
                                    }
                                    player_spawn_z = world_info.spawn_z;
                                    printf("Using real-world spawn at (%d,%d,%d)%s.\n",
                                           player_spawn_x,
                                           player_spawn_y,
                                           player_spawn_z,
                                           have_safe_spawn ? " from WORLD_SURFACE" : " with high-alt fallback");
                                } else {
                                    player_spawn_x = debug_spawn_x;
                                    player_spawn_y = debug_spawn_y;
                                    player_spawn_z = debug_spawn_z;
                                    printf("Using debug spawn at (%d,%d,%d).\n", player_spawn_x, player_spawn_y, player_spawn_z);
                                }
                            }

                                                        // Move the client's loading area to the real world spawn chunk when available.
                            if (has_world_info) {
                                uint8_t spawn_buf[128];
                                size_t spawn_len = build_set_default_spawn_packet(
                                    spawn_buf,
                                    sizeof(spawn_buf),
                                    world_info.dimension_name,
                                    player_spawn_x,
                                    player_spawn_y,
                                    player_spawn_z,
                                    world_info.spawn_yaw,
                                    world_info.spawn_pitch);
                                send_post_compression_packet(new_socket, spawn_buf, spawn_len);
                                printf("Sent Default Spawn Position.\n");
                            } else {
                                uint8_t spawn_buf[128];
                                size_t spawn_len = build_set_default_spawn_packet(
                                    spawn_buf,
                                    sizeof(spawn_buf),
                                    "minecraft:overworld",
                                    player_spawn_x,
                                    player_spawn_y,
                                    player_spawn_z,
                                    0.0f,
                                    0.0f);
                                send_post_compression_packet(new_socket, spawn_buf, spawn_len);
                                printf("Sent Default Spawn Position fallback.\n");
                            }

                            {
                                uint8_t time_buf[64];
                                size_t time_len = build_update_time_packet(time_buf, sizeof(time_buf), 0, 1000, 1);
                                send_post_compression_packet(new_socket, time_buf, time_len);
                                printf("Sent Update Time.\n");
                            }

                            // Player Position and Look
                            uint8_t pos_buf[128];
                            player_pos_params_t pos_params;
                            memset(&pos_params, 0, sizeof(pos_params));
                            pos_params.teleport_id = 1;
                            pos_params.x = (double)player_spawn_x + 0.5;
                            pos_params.y = (double)player_spawn_y;
                            pos_params.z = (double)player_spawn_z + 0.5;
                            pos_params.yaw = (!force_debug_spawn && has_world_info) ? world_info.spawn_yaw : 0.0f;
                            pos_params.pitch = (!force_debug_spawn && has_world_info) ? world_info.spawn_pitch : 0.0f;
                            size_t pos_len = build_player_pos_packet_ex(pos_buf, sizeof(pos_buf), &pos_params);
                            send_post_compression_packet(new_socket, pos_buf, pos_len);
                            printf("Sent Player Position and Look to client.\n");

                            // Keep the socket open in play state, send Keep Alive every 10s, and log inbound packets.
#ifdef _WIN32
                            {
                                DWORD timeout_ms = 1000;
                                setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));
                            }
#else
                            {
                                struct timeval tv;
                                tv.tv_sec = 1;
                                tv.tv_usec = 0;
                                setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                            }
#endif
                            {
                                int entered_play_state = 1;
#ifdef _WIN32
                                u_long listener_nonblock_mode = 1;
                                ioctlsocket(server_fd, FIONBIO, &listener_nonblock_mode);
#else
                                int listener_old_flags = fcntl(server_fd, F_GETFL, 0);
                                if (listener_old_flags >= 0) {
                                    fcntl(server_fd, F_SETFL, listener_old_flags | O_NONBLOCK);
                                }
#endif
                                increment_connected_play_sessions();
                                time_t last_keepalive = time(NULL);
                                int64_t ka_id = 1;
                                for (;;) {
                                    // Keep status ping responsive while a play session is active.
                                    try_serve_one_pending_status(server_fd, &server_config);

                                    // Send Keep Alive every 10 seconds
                                    time_t now = time(NULL);
                                    if (now - last_keepalive >= 10) {
                                        uint8_t ka_buf[32];
                                        size_t ka_len = build_keep_alive_packet(ka_buf, sizeof(ka_buf), ka_id++);
                                        send_post_compression_packet(new_socket, ka_buf, ka_len);
                                        printf("Sent Keep Alive (id=%lld).\n", (long long)(ka_id - 1));
                                        last_keepalive = now;
                                    }

                                    uint8_t play_buf[2048];
#ifdef _WIN32
                                    int play_bytes = recv(new_socket, (char*)play_buf, sizeof(play_buf), 0);
                                    if (play_bytes <= 0) {
                                        if (WSAGetLastError() == WSAETIMEDOUT) continue;
                                        break;
                                    }
#else
                                    ssize_t play_bytes = recv(new_socket, play_buf, sizeof(play_buf), 0);
                                    if (play_bytes <= 0) {
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                                        break;
                                    }
#endif
                                    {
                                        int32_t play_pid = -1;
                                        const uint8_t *play_payload = NULL;
                                        size_t play_payload_len = 0;
                                        uint8_t inflate_tmp[8192];

                                        if (read_post_compression_packet_data(
                                                play_buf,
                                                (size_t)play_bytes,
                                                &play_pid,
                                                &play_payload,
                                                &play_payload_len,
                                                inflate_tmp,
                                                sizeof(inflate_tmp))) {
                                            if (g_log_play_packets) {
                                                printf("Play packet ID from client: %d\n", play_pid);
                                            }

                                            {
                                                double player_x = 0.0;
                                                double player_z = 0.0;
                                                if (try_extract_position_from_play_packet(
                                                        play_pid,
                                                        play_payload,
                                                        play_payload_len,
                                                        &player_x,
                                                        &player_z)) {
                                                    int32_t moved_chunk_x = (int32_t)floor(player_x / 16.0);
                                                    int32_t moved_chunk_z = (int32_t)floor(player_z / 16.0);

                                                    if (moved_chunk_x != stream_state.stream_center_chunk_x || moved_chunk_z != stream_state.stream_center_chunk_z) {
                                                        uint8_t center_buf[64];
                                                        size_t center_len = build_set_center_chunk_packet(
                                                            center_buf,
                                                            sizeof(center_buf),
                                                            moved_chunk_x,
                                                            moved_chunk_z);
                                                        send_post_compression_packet(new_socket, center_buf, center_len);

                                                        {
                                                            uint8_t cbs_buf[4];
                                                            size_t cbs_len = 0;
                                                            int sent_chunks = 0;
                                                            uint8_t cbf_buf[8];
                                                            size_t cbf_len = 0;

                                                            cbs_len += write_varint(cbs_buf + cbs_len, 0x0C);
                                                            send_post_compression_packet(new_socket, cbs_buf, cbs_len);

                                                            for (int dz = -stream_state.chunk_stream_radius; dz <= stream_state.chunk_stream_radius; dz++) {
                                                                for (int dx = -stream_state.chunk_stream_radius; dx <= stream_state.chunk_stream_radius; dx++) {
                                                                    int32_t sx = moved_chunk_x + dx;
                                                                    int32_t sz = moved_chunk_z + dz;
                                                                    int already_loaded =
                                                                        abs(sx - stream_state.stream_center_chunk_x) <= stream_state.chunk_stream_radius &&
                                                                        abs(sz - stream_state.stream_center_chunk_z) <= stream_state.chunk_stream_radius;

                                                                    if (already_loaded) {
                                                                        continue;
                                                                    }

                                                                    if (send_stream_chunk(new_socket,
                                                                  sx,
                                                                  sz,
                                                                  &server_config,
                                                                  stream_state.force_debug_spawn,
                                                                  stream_state.has_world_info,
                                                                  &world_info)) {
                                                                        sent_chunks += 1;
                                                                    }
                                                                }
                                                            }

                                                            cbf_len += write_varint(cbf_buf + cbf_len, 0x0B);
                                                            cbf_len += write_varint(cbf_buf + cbf_len, sent_chunks);
                                                            send_post_compression_packet(new_socket, cbf_buf, cbf_len);
                                                        }

                                                        stream_state.stream_center_chunk_x = moved_chunk_x;
                                                        stream_state.stream_center_chunk_z = moved_chunk_z;
                                                    }
                                                }
                                            }
                                        } else {
                                            if (g_log_packet_framing) {
                                                printf("Failed to parse play packet in post-compression format.\n");
                                            }
                                        }
                                    }
                                }

                                if (entered_play_state) {
                                    decrement_connected_play_sessions();
                                }
#ifdef _WIN32
                                listener_nonblock_mode = 0;
                                ioctlsocket(server_fd, FIONBIO, &listener_nonblock_mode);
#else
                                if (listener_old_flags >= 0) {
                                    fcntl(server_fd, F_SETFL, listener_old_flags);
                                }
#endif
                            }

                            if (has_world_info) {
                                free_world_info(&world_info);
                            }

#ifdef _WIN32
                            closesocket(new_socket);
#else
                            close(new_socket);
#endif
                        } else {
                            printf("Did not receive Login Acknowledged (got id=%d)\n", ackpid);
                        }
                    } else {
                        printf("No packet received after Login Success.\n");
                    }
                    free(username);
                }
            }
        } else {
#ifdef _WIN32
            closesocket(new_socket);
#else
            close(new_socket);
#endif
        }
    }

#ifdef _WIN32
    closesocket(server_fd);
    WSACleanup();
#endif
    return 0;
}

// Helper to frame a packet with VarInt uncompressed length prefix
static size_t frame_packet(uint8_t *dst, const uint8_t *src, size_t len) {
    size_t off = 0;
    off += write_varint(dst + off, (int)len);
    memcpy(dst + off, src, len);
    return off + len;
}

