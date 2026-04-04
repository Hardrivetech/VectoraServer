#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include "protocol.h"
#include "packet.h"
#include "configuration_packets.h"
#include "configuration_replay.h"
#include "server_config.h"
#include "status.h"
#include "join_game.h"
#include "player_position.h"
#include "play_packets.h"
#include "entity_registry.h"
#include "entity_manager.h"
#include "world_loader.h"
#include "chunk_sender.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <direct.h>
#include <sys/stat.h>
#include <stddef.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_handle_t;
#else
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
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
#define MAX_PLAY_CHAT_SOCKETS 128
#define MAX_MUTED_PLAYERS 128
#define MAX_MODERATORS 64
#define MODERATORS_FILE_PATH "moderators.txt"
#define MUTED_FILE_PATH "muted.txt"
#define MOD_AUDIT_LOG_FILE_PATH "moderation.log"

#define PLAY774_C2S_CHAT_COMMAND        0x06
#define PLAY774_C2S_CHAT_COMMAND_SIGNED 0x07
#define PLAY774_C2S_CHAT_MESSAGE        0x08
#define PLAY774_C2S_BLOCK_DIG           0x28
#define PLAY774_S2C_SYSTEM_CHAT         0x77
#define PLAY774_ENTITY_TYPE_ITEM        71
#define PLAY774_ITEM_ID_STONE           1
#define PLAY774_BLOCK_STATE_AIR         0
#define PLAY774_BLOCK_DIG_START         0
#define PLAY774_BLOCK_DIG_FINISH        2
#define CHAT_RATE_MAX_MESSAGES          5
#define CHAT_RATE_WINDOW_SECONDS        3
#define CHAT_RATE_BLOCK_BASE_SECONDS    10
#define CHAT_RATE_BLOCK_MAX_SECONDS     120
#define CHAT_RATE_STRIKE_DECAY_SECONDS  300
#define GENERATED_PACKET_CACHE_VERSION  18
#define DROP_DEDUP_CACHE_SIZE           64
#define DROP_DEDUP_WINDOW_SECONDS       2
#define BROKEN_BLOCK_CACHE_SIZE         32768

#ifdef _WIN32
static socket_handle_t g_listen_socket = INVALID_SOCKET;
#else
static socket_handle_t g_listen_socket = -1;
#endif

static void append_lifecycle_log(const char *fmt, ...) {
    FILE *f;
    va_list ap;
    char ts[32];
    time_t now;
    struct tm tm_local;

    f = fopen("lifecycle.log", "a");
    if (f == NULL) {
        return;
    }

    now = time(NULL);
#ifdef _WIN32
    localtime_s(&tm_local, &now);
#else
    localtime_r(&now, &tm_local);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_local);

#ifdef _WIN32
    fprintf(f, "[%s pid=%lu] ", ts, (unsigned long)GetCurrentProcessId());
#else
    fprintf(f, "[%s pid=%ld] ", ts, (long)getpid());
#endif

    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}

static socket_handle_t g_play_chat_sockets[MAX_PLAY_CHAT_SOCKETS];
static char g_play_chat_socket_usernames[MAX_PLAY_CHAT_SOCKETS][32];
static size_t g_play_chat_socket_count = 0;
static char g_muted_usernames[MAX_MUTED_PLAYERS][32];
static size_t g_muted_username_count = 0;
static char g_moderator_usernames[MAX_MODERATORS][32];
static size_t g_moderator_username_count = 0;
#ifdef _WIN32
static CRITICAL_SECTION g_play_chat_lock;
static int g_play_chat_lock_initialized = 0;
static void init_play_chat_lock(void) {
    if (!g_play_chat_lock_initialized) {
        InitializeCriticalSection(&g_play_chat_lock);
        g_play_chat_lock_initialized = 1;
    }
}
static void destroy_play_chat_lock(void) {
    if (g_play_chat_lock_initialized) {
        DeleteCriticalSection(&g_play_chat_lock);
        g_play_chat_lock_initialized = 0;
    }
}
#define PLAY_CHAT_LOCK() EnterCriticalSection(&g_play_chat_lock)
#define PLAY_CHAT_UNLOCK() LeaveCriticalSection(&g_play_chat_lock)
#else
static pthread_mutex_t g_play_chat_lock = PTHREAD_MUTEX_INITIALIZER;
static void init_play_chat_lock(void) {}
static void destroy_play_chat_lock(void) {}
#define PLAY_CHAT_LOCK() pthread_mutex_lock(&g_play_chat_lock)
#define PLAY_CHAT_UNLOCK() pthread_mutex_unlock(&g_play_chat_lock)
#endif

#ifdef _WIN32
static volatile LONG g_connected_play_sessions = 0;
static volatile LONG g_active_connections = 0;

static LONG WINAPI vectora_unhandled_exception_filter(EXCEPTION_POINTERS *ep) {
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    append_lifecycle_log("[FATAL] Unhandled process exception code=0x%08lX", (unsigned long)code);
    fprintf(stderr, "[FATAL] Unhandled process exception (code=0x%08lX).\n", (unsigned long)code);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void vectora_invalid_parameter_handler(const wchar_t *expression,
                                              const wchar_t *function,
                                              const wchar_t *file,
                                              unsigned int line,
                                              uintptr_t pReserved) {
    (void)expression;
    (void)function;
    (void)file;
    (void)pReserved;
    append_lifecycle_log("[FATAL] CRT invalid parameter handler triggered line=%u", line);
    fprintf(stderr, "[FATAL] CRT invalid parameter handler triggered (line=%u).\n", line);
    fflush(stderr);
}

static void vectora_signal_handler(int sig) {
    append_lifecycle_log("[FATAL] Signal handler invoked signal=%d", sig);
    fprintf(stderr, "[FATAL] Signal handler invoked (signal=%d).\n", sig);
    fflush(stderr);
}

static void vectora_process_exit_marker(void) {
    append_lifecycle_log("[INFO] Process exiting via normal atexit path");
    fprintf(stderr, "[INFO] Process exiting via normal atexit path.\n");
    fflush(stderr);
}

static int load_connected_play_sessions(void) {
    return (int)InterlockedCompareExchange(&g_connected_play_sessions, 0, 0);
}
static void increment_connected_play_sessions(void) {
    InterlockedIncrement(&g_connected_play_sessions);
}
static void decrement_connected_play_sessions(void) {
    InterlockedDecrement(&g_connected_play_sessions);
}
static int load_active_connections(void) {
    return (int)InterlockedCompareExchange(&g_active_connections, 0, 0);
}
static void increment_active_connections(void) {
    InterlockedIncrement(&g_active_connections);
}
static void decrement_active_connections(void) {
    InterlockedDecrement(&g_active_connections);
}
#else
static volatile int g_connected_play_sessions = 0;
static volatile int g_active_connections = 0;
static int load_connected_play_sessions(void) {
    return __sync_add_and_fetch(&g_connected_play_sessions, 0);
}
static void increment_connected_play_sessions(void) {
    __sync_add_and_fetch(&g_connected_play_sessions, 1);
}
static void decrement_connected_play_sessions(void) {
    __sync_sub_and_fetch(&g_connected_play_sessions, 1);
}
static int load_active_connections(void) {
    return __sync_add_and_fetch(&g_active_connections, 0);
}
static void increment_active_connections(void) {
    __sync_add_and_fetch(&g_active_connections, 1);
}
static void decrement_active_connections(void) {
    __sync_sub_and_fetch(&g_active_connections, 1);
}
#endif

typedef struct {
    int32_t stream_center_chunk_x;
    int32_t stream_center_chunk_z;
    int chunk_stream_radius;
    int force_debug_spawn;
    int has_world_info;
} client_stream_state_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
    time_t at;
} recent_drop_entry_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
    time_t at;
} broken_block_entry_t;

typedef struct {
    socket_handle_t socket_fd;
    socket_handle_t server_fd;
    server_config_t server_config;
    char username[32];
    int32_t client_protocol_version;
    client_stream_state_t stream_state;
    entity_registry_t entity_registry;
    entity_manager_t entity_manager;
    int32_t mock_entity_a;
    int32_t mock_entity_b;
    int mock_entity_a_visible;
    int mock_entity_b_visible;
    int32_t spawned_entity_ids[256];
    int32_t spawned_entity_types[256];
    int spawned_entity_visible[256];
    time_t spawned_entity_birth_at[256];
    double spawned_entity_vertical_velocity[256];
    int spawned_entity_on_ground[256];
    size_t spawned_entity_count;
    uint64_t entity_stats_spawn_packets_sent;
    uint64_t entity_stats_destroy_packets_sent;
    uint64_t entity_stats_move_packets_sent;
    uint64_t entity_stats_head_packets_sent;
    uint64_t entity_stats_update_culled;
    time_t chat_rate_window_start;
    int chat_rate_message_count;
    time_t chat_rate_block_until;
    int chat_rate_strike_count;
    time_t chat_rate_last_violation;
    recent_drop_entry_t recent_drops[DROP_DEDUP_CACHE_SIZE];
    size_t recent_drop_count;
    broken_block_entry_t broken_blocks[BROKEN_BLOCK_CACHE_SIZE];
    size_t broken_block_count;
    time_t last_broken_block_reconcile_at;
} client_session_t;

typedef enum {
    CHUNK_SEND_RESULT_NONE = 0,
    CHUNK_SEND_RESULT_REAL = 1,
    CHUNK_SEND_RESULT_GENERATED = 2,
    CHUNK_SEND_RESULT_DEBUG = 3
} chunk_send_result_t;

static int resolve_generated_world_root(const server_config_t *server_config,
                                        char *world_root,
                                        size_t world_root_size);
static int ensure_generated_world_region_chunk_stub(const char *world_root,
                                                    int32_t chunk_x,
                                                    int32_t chunk_z);
static int load_cached_generated_chunk_packet(const server_config_t *server_config,
                                              int32_t chunk_x,
                                              int32_t chunk_z,
                                              uint8_t **out_packet,
                                              size_t *out_packet_len);
static void save_cached_generated_chunk_packet(const server_config_t *server_config,
                                               int32_t chunk_x,
                                               int32_t chunk_z,
                                               const uint8_t *packet,
                                               size_t packet_len);
static int build_generated_chunk_nbt_payload(int32_t chunk_x,
                                             int32_t chunk_z,
                                             uint8_t **out_nbt,
                                             size_t *out_nbt_len);
static void send_post_compression_packet(socket_handle_t socket_fd,
                                         const uint8_t *packet,
                                         size_t packet_len);

static const char *world_source_mode_name(int mode) {
    switch (mode) {
        case WORLD_SOURCE_MODE_AUTO: return "auto";
        case WORLD_SOURCE_MODE_REAL: return "real";
        case WORLD_SOURCE_MODE_GENERATED: return "generated";
        case WORLD_SOURCE_MODE_DEBUG: return "debug";
        default: return "unknown";
    }
}

static int track_spawned_entity_id(client_session_t *session, int32_t entity_id, int32_t entity_type) {
    size_t i;

    if (session == NULL) {
        return 0;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        if (session->spawned_entity_ids[i] == entity_id) {
            return 1;
        }
    }

    if (session->spawned_entity_count >= (sizeof(session->spawned_entity_ids) / sizeof(session->spawned_entity_ids[0]))) {
        return 0;
    }

    session->spawned_entity_ids[session->spawned_entity_count] = entity_id;
    session->spawned_entity_types[session->spawned_entity_count] = entity_type;
    session->spawned_entity_visible[session->spawned_entity_count] = 1;
    session->spawned_entity_birth_at[session->spawned_entity_count] = time(NULL);
    session->spawned_entity_vertical_velocity[session->spawned_entity_count] = 0.0;
    session->spawned_entity_on_ground[session->spawned_entity_count] = 0;
    session->spawned_entity_count += 1;
    return 1;
}

static int untrack_spawned_entity_id(client_session_t *session, int32_t entity_id) {
    size_t i;

    if (session == NULL) {
        return 0;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        if (session->spawned_entity_ids[i] == entity_id) {
            size_t tail = session->spawned_entity_count - i - 1;
            if (tail > 0) {
                memmove(&session->spawned_entity_ids[i],
                        &session->spawned_entity_ids[i + 1],
                        tail * sizeof(session->spawned_entity_ids[0]));
                memmove(&session->spawned_entity_types[i],
                    &session->spawned_entity_types[i + 1],
                    tail * sizeof(session->spawned_entity_types[0]));
                memmove(&session->spawned_entity_visible[i],
                    &session->spawned_entity_visible[i + 1],
                    tail * sizeof(session->spawned_entity_visible[0]));
                memmove(&session->spawned_entity_birth_at[i],
                    &session->spawned_entity_birth_at[i + 1],
                    tail * sizeof(session->spawned_entity_birth_at[0]));
                memmove(&session->spawned_entity_vertical_velocity[i],
                    &session->spawned_entity_vertical_velocity[i + 1],
                    tail * sizeof(session->spawned_entity_vertical_velocity[0]));
                memmove(&session->spawned_entity_on_ground[i],
                    &session->spawned_entity_on_ground[i + 1],
                    tail * sizeof(session->spawned_entity_on_ground[0]));
            }
            session->spawned_entity_count -= 1;
            return 1;
        }
    }

    return 0;
}

static int is_tracked_spawned_entity_id(const client_session_t *session, int32_t entity_id) {
    size_t i;

    if (session == NULL) {
        return 0;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        if (session->spawned_entity_ids[i] == entity_id) {
            return 1;
        }
    }

    return 0;
}

static int find_tracked_spawned_entity_index(const client_session_t *session, int32_t entity_id) {
    size_t i;

    if (session == NULL) {
        return -1;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        if (session->spawned_entity_ids[i] == entity_id) {
            return (int)i;
        }
    }

    return -1;
}

static int should_spawn_block_drop(client_session_t *session,
                                   int32_t block_x,
                                   int32_t block_y,
                                   int32_t block_z,
                                   time_t now_ts) {
    size_t i;
    size_t oldest_index = 0;
    time_t oldest_time = now_ts;

    if (session == NULL) {
        return 0;
    }

    if (session->recent_drop_count == 0) {
        session->recent_drops[0].x = block_x;
        session->recent_drops[0].y = block_y;
        session->recent_drops[0].z = block_z;
        session->recent_drops[0].at = now_ts;
        session->recent_drop_count = 1;
        return 1;
    }

    oldest_time = session->recent_drops[0].at;
    for (i = 0; i < session->recent_drop_count; ++i) {
        recent_drop_entry_t *entry = &session->recent_drops[i];

        if (entry->x == block_x && entry->y == block_y && entry->z == block_z) {
            if ((now_ts - entry->at) <= DROP_DEDUP_WINDOW_SECONDS) {
                return 0;
            }
            entry->at = now_ts;
            return 1;
        }

        if (entry->at < oldest_time) {
            oldest_time = entry->at;
            oldest_index = i;
        }
    }

    if (session->recent_drop_count < DROP_DEDUP_CACHE_SIZE) {
        session->recent_drops[session->recent_drop_count].x = block_x;
        session->recent_drops[session->recent_drop_count].y = block_y;
        session->recent_drops[session->recent_drop_count].z = block_z;
        session->recent_drops[session->recent_drop_count].at = now_ts;
        session->recent_drop_count += 1;
        return 1;
    }

    session->recent_drops[oldest_index].x = block_x;
    session->recent_drops[oldest_index].y = block_y;
    session->recent_drops[oldest_index].z = block_z;
    session->recent_drops[oldest_index].at = now_ts;
    return 1;
}

static int is_block_marked_broken(const client_session_t *session,
                                  int32_t block_x,
                                  int32_t block_y,
                                  int32_t block_z) {
    size_t i;

    if (session == NULL) {
        return 0;
    }

    for (i = 0; i < session->broken_block_count; ++i) {
        const broken_block_entry_t *entry = &session->broken_blocks[i];
        if (entry->x == block_x && entry->y == block_y && entry->z == block_z) {
            return 1;
        }
    }

    return 0;
}

static void mark_block_broken(client_session_t *session,
                              int32_t block_x,
                              int32_t block_y,
                              int32_t block_z,
                              time_t now_ts) {
    size_t i;
    size_t oldest_index = 0;
    time_t oldest_time = now_ts;

    if (session == NULL) {
        return;
    }

    for (i = 0; i < session->broken_block_count; ++i) {
        broken_block_entry_t *entry = &session->broken_blocks[i];
        if (entry->x == block_x && entry->y == block_y && entry->z == block_z) {
            entry->at = now_ts;
            return;
        }
    }

    if (session->broken_block_count < BROKEN_BLOCK_CACHE_SIZE) {
        broken_block_entry_t *entry = &session->broken_blocks[session->broken_block_count++];
        entry->x = block_x;
        entry->y = block_y;
        entry->z = block_z;
        entry->at = now_ts;
        return;
    }

    oldest_time = session->broken_blocks[0].at;
    for (i = 1; i < session->broken_block_count; ++i) {
        if (session->broken_blocks[i].at < oldest_time) {
            oldest_time = session->broken_blocks[i].at;
            oldest_index = i;
        }
    }

    session->broken_blocks[oldest_index].x = block_x;
    session->broken_blocks[oldest_index].y = block_y;
    session->broken_blocks[oldest_index].z = block_z;
    session->broken_blocks[oldest_index].at = now_ts;
}

static int block_to_chunk_coord(int32_t block_coord) {
    if (block_coord >= 0) {
        return block_coord / 16;
    }
    return (block_coord - 15) / 16;
}

static void replay_broken_blocks_for_chunk(client_session_t *session,
                                           int32_t chunk_x,
                                           int32_t chunk_z) {
    size_t i;

    if (session == NULL || session->broken_block_count == 0) {
        return;
    }

    for (i = 0; i < session->broken_block_count; ++i) {
        const broken_block_entry_t *entry = &session->broken_blocks[i];
        if (block_to_chunk_coord(entry->x) == chunk_x &&
            block_to_chunk_coord(entry->z) == chunk_z) {
            uint8_t bu_buf[32];
            size_t bu_len = build_block_update_packet(bu_buf,
                                                      sizeof(bu_buf),
                                                      entry->x,
                                                      entry->y,
                                                      entry->z,
                                                      PLAY774_BLOCK_STATE_AIR);
            send_post_compression_packet(session->socket_fd, bu_buf, bu_len);
        }
    }
}

static void reconcile_broken_blocks_near_player(client_session_t *session,
                                                double player_x,
                                                double player_z) {
    const double max_dist_sq = 192.0 * 192.0;
    size_t n;
    int sent = 0;

    if (session == NULL || session->broken_block_count == 0) {
        return;
    }

    for (n = session->broken_block_count; n > 0; --n) {
        const broken_block_entry_t *entry = &session->broken_blocks[n - 1];
        double dx = ((double)entry->x + 0.5) - player_x;
        double dz = ((double)entry->z + 0.5) - player_z;
        double dist_sq = dx * dx + dz * dz;

        if (dist_sq <= max_dist_sq) {
            uint8_t bu_buf[32];
            size_t bu_len = build_block_update_packet(bu_buf,
                                                      sizeof(bu_buf),
                                                      entry->x,
                                                      entry->y,
                                                      entry->z,
                                                      PLAY774_BLOCK_STATE_AIR);
            send_post_compression_packet(session->socket_fd, bu_buf, bu_len);
            sent += 1;
            if (sent >= 256) {
                break;
            }
        }
    }
}

static int ensure_tracked_slot_for_drop(client_session_t *session,
                                        socket_handle_t socket_fd) {
    size_t i;
    size_t oldest_item_index = SIZE_MAX;
    size_t oldest_any_index = SIZE_MAX;
    time_t oldest_item_birth = 0;
    time_t oldest_any_birth = 0;
    size_t tracked_cap;

    if (session == NULL) {
        return 0;
    }

    tracked_cap = sizeof(session->spawned_entity_ids) / sizeof(session->spawned_entity_ids[0]);
    if (session->spawned_entity_count < tracked_cap) {
        return 1;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        time_t born = session->spawned_entity_birth_at[i];

        if (oldest_any_index == SIZE_MAX || born < oldest_any_birth) {
            oldest_any_index = i;
            oldest_any_birth = born;
        }

        if (session->spawned_entity_types[i] == PLAY774_ENTITY_TYPE_ITEM) {
            if (oldest_item_index == SIZE_MAX || born < oldest_item_birth) {
                oldest_item_index = i;
                oldest_item_birth = born;
            }
        }
    }

    if (oldest_item_index != SIZE_MAX || oldest_any_index != SIZE_MAX) {
        size_t remove_index = (oldest_item_index != SIZE_MAX) ? oldest_item_index : oldest_any_index;
        int32_t remove_id = session->spawned_entity_ids[remove_index];
        int remove_visible = session->spawned_entity_visible[remove_index];

        if (remove_visible) {
            uint8_t rem_buf[64];
            size_t rem_len = build_entity_destroy_packet(rem_buf, sizeof(rem_buf), &remove_id, 1);
            send_post_compression_packet(socket_fd, rem_buf, rem_len);
            session->entity_stats_destroy_packets_sent += 1;
        }

        (void)entity_manager_queue_remove(&session->entity_registry,
                                          &session->entity_manager,
                                          remove_id);
        (void)untrack_spawned_entity_id(session, remove_id);
    }

    return session->spawned_entity_count < tracked_cap;
}

static int username_equals_ci(const char *a, const char *b) {
    size_t i = 0;

    if (a == NULL || b == NULL) {
        return 0;
    }

    while (a[i] != '\0' && b[i] != '\0') {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
        ++i;
    }

    return a[i] == '\0' && b[i] == '\0';
}

static int is_username_muted(const char *username) {
    size_t i;
    int muted = 0;

    if (username == NULL || username[0] == '\0') {
        return 0;
    }

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_muted_username_count; ++i) {
        if (username_equals_ci(g_muted_usernames[i], username)) {
            muted = 1;
            break;
        }
    }
    PLAY_CHAT_UNLOCK();

    return muted;
}

static int add_muted_username(const char *username) {
    size_t i;

    if (username == NULL || username[0] == '\0') {
        return 0;
    }

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_muted_username_count; ++i) {
        if (username_equals_ci(g_muted_usernames[i], username)) {
            PLAY_CHAT_UNLOCK();
            return 0;
        }
    }

    if (g_muted_username_count >= MAX_MUTED_PLAYERS) {
        PLAY_CHAT_UNLOCK();
        return 0;
    }

    snprintf(g_muted_usernames[g_muted_username_count],
             sizeof(g_muted_usernames[g_muted_username_count]),
             "%s",
             username);
    g_muted_username_count += 1;
    PLAY_CHAT_UNLOCK();
    return 1;
}

static int remove_muted_username(const char *username) {
    size_t i;

    if (username == NULL || username[0] == '\0') {
        return 0;
    }

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_muted_username_count; ++i) {
        if (username_equals_ci(g_muted_usernames[i], username)) {
            size_t tail = g_muted_username_count - i - 1;
            if (tail > 0) {
                memmove(&g_muted_usernames[i],
                        &g_muted_usernames[i + 1],
                        tail * sizeof(g_muted_usernames[0]));
            }
            g_muted_username_count -= 1;
            PLAY_CHAT_UNLOCK();
            return 1;
        }
    }
    PLAY_CHAT_UNLOCK();

    return 0;
}

static int is_username_moderator(const char *username) {
    size_t i;
    int is_mod = 0;

    if (username == NULL || username[0] == '\0') {
        return 0;
    }

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_moderator_username_count; ++i) {
        if (username_equals_ci(g_moderator_usernames[i], username)) {
            is_mod = 1;
            break;
        }
    }
    PLAY_CHAT_UNLOCK();

    return is_mod;
}

static int add_moderator_username(const char *username) {
    size_t i;

    if (username == NULL || username[0] == '\0') {
        return 0;
    }

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_moderator_username_count; ++i) {
        if (username_equals_ci(g_moderator_usernames[i], username)) {
            PLAY_CHAT_UNLOCK();
            return 0;
        }
    }

    if (g_moderator_username_count >= MAX_MODERATORS) {
        PLAY_CHAT_UNLOCK();
        return 0;
    }

    snprintf(g_moderator_usernames[g_moderator_username_count],
             sizeof(g_moderator_usernames[g_moderator_username_count]),
             "%s",
             username);
    g_moderator_username_count += 1;
    PLAY_CHAT_UNLOCK();
    return 1;
}

/* return 1 on removed, 0 if not found, -1 if operation would remove last moderator */
static int remove_moderator_username(const char *username) {
    size_t i;

    if (username == NULL || username[0] == '\0') {
        return 0;
    }

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_moderator_username_count; ++i) {
        if (username_equals_ci(g_moderator_usernames[i], username)) {
            size_t tail;
            if (g_moderator_username_count <= 1) {
                PLAY_CHAT_UNLOCK();
                return -1;
            }
            tail = g_moderator_username_count - i - 1;
            if (tail > 0) {
                memmove(&g_moderator_usernames[i],
                        &g_moderator_usernames[i + 1],
                        tail * sizeof(g_moderator_usernames[0]));
            }
            g_moderator_username_count -= 1;
            PLAY_CHAT_UNLOCK();
            return 1;
        }
    }
    PLAY_CHAT_UNLOCK();

    return 0;
}

static int ensure_initial_moderator(const char *username) {
    int promoted = 0;

    if (username == NULL || username[0] == '\0') {
        return 0;
    }

    PLAY_CHAT_LOCK();
    if (g_moderator_username_count == 0 && g_moderator_username_count < MAX_MODERATORS) {
        snprintf(g_moderator_usernames[g_moderator_username_count],
                 sizeof(g_moderator_usernames[g_moderator_username_count]),
                 "%s",
                 username);
        g_moderator_username_count += 1;
        promoted = 1;
    }
    PLAY_CHAT_UNLOCK();

    return promoted;
}

static int save_username_list_to_file(const char *path,
                                      char list[][32],
                                      size_t count) {
    FILE *fp;
    size_t i;

    if (path == NULL) {
        return 0;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        return 0;
    }

    for (i = 0; i < count; ++i) {
        if (list[i][0] == '\0') {
            continue;
        }
        fputs(list[i], fp);
        fputc('\n', fp);
    }

    fclose(fp);
    return 1;
}

static size_t load_username_list_from_file(const char *path,
                                           char list[][32],
                                           size_t max_count) {
    FILE *fp;
    char line[256];
    size_t count = 0;

    if (path == NULL) {
        return 0;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL && count < max_count) {
        char name[32];
        size_t n;

        n = strcspn(line, "\r\n");
        line[n] = '\0';
        while (line[0] == ' ' || line[0] == '\t') {
            memmove(line, line + 1, strlen(line));
        }
        while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) {
            line[n - 1] = '\0';
            --n;
        }

        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        snprintf(name, sizeof(name), "%s", line);
        {
            size_t i;
            int dup = 0;
            for (i = 0; i < count; ++i) {
                if (username_equals_ci(list[i], name)) {
                    dup = 1;
                    break;
                }
            }
            if (!dup) {
                snprintf(list[count], sizeof(list[count]), "%s", name);
                count += 1;
            }
        }
    }

    fclose(fp);
    return count;
}

static int save_moderation_state(void) {
    char moderators_snapshot[MAX_MODERATORS][32];
    char muted_snapshot[MAX_MUTED_PLAYERS][32];
    size_t moderators_count = 0;
    size_t muted_count = 0;
    size_t i;
    int ok_mod;
    int ok_muted;

    PLAY_CHAT_LOCK();
    moderators_count = g_moderator_username_count;
    muted_count = g_muted_username_count;
    if (moderators_count > MAX_MODERATORS) {
        moderators_count = MAX_MODERATORS;
    }
    if (muted_count > MAX_MUTED_PLAYERS) {
        muted_count = MAX_MUTED_PLAYERS;
    }

    for (i = 0; i < moderators_count; ++i) {
        snprintf(moderators_snapshot[i], sizeof(moderators_snapshot[i]), "%s", g_moderator_usernames[i]);
    }
    for (i = 0; i < muted_count; ++i) {
        snprintf(muted_snapshot[i], sizeof(muted_snapshot[i]), "%s", g_muted_usernames[i]);
    }
    PLAY_CHAT_UNLOCK();

    ok_mod = save_username_list_to_file(MODERATORS_FILE_PATH, moderators_snapshot, moderators_count);
    ok_muted = save_username_list_to_file(MUTED_FILE_PATH, muted_snapshot, muted_count);
    return ok_mod && ok_muted;
}

static void load_moderation_state(void) {
    char moderators_loaded[MAX_MODERATORS][32] = {0};
    char muted_loaded[MAX_MUTED_PLAYERS][32] = {0};
    size_t moderators_count;
    size_t muted_count;
    size_t i;

    moderators_count = load_username_list_from_file(MODERATORS_FILE_PATH,
                                                    moderators_loaded,
                                                    MAX_MODERATORS);
    muted_count = load_username_list_from_file(MUTED_FILE_PATH,
                                               muted_loaded,
                                               MAX_MUTED_PLAYERS);

    PLAY_CHAT_LOCK();
    g_moderator_username_count = 0;
    g_muted_username_count = 0;

    for (i = 0; i < moderators_count; ++i) {
        snprintf(g_moderator_usernames[g_moderator_username_count],
                 sizeof(g_moderator_usernames[g_moderator_username_count]),
                 "%s",
                 moderators_loaded[i]);
        g_moderator_username_count += 1;
    }

    for (i = 0; i < muted_count; ++i) {
        snprintf(g_muted_usernames[g_muted_username_count],
                 sizeof(g_muted_usernames[g_muted_username_count]),
                 "%s",
                 muted_loaded[i]);
        g_muted_username_count += 1;
    }
    PLAY_CHAT_UNLOCK();
}

static void append_moderation_audit(const char *actor,
                                    const char *action,
                                    const char *target,
                                    const char *details) {
    FILE *fp;
    time_t now;
    struct tm tmv;
    char timestamp[32];
    const char *actor_name = (actor != NULL && actor[0] != '\0') ? actor : "SYSTEM";
    const char *action_name = (action != NULL && action[0] != '\0') ? action : "UNKNOWN";
    const char *target_name = (target != NULL && target[0] != '\0') ? target : "-";
    const char *detail_text = (details != NULL && details[0] != '\0') ? details : "";

    fp = fopen(MOD_AUDIT_LOG_FILE_PATH, "ab");
    if (fp == NULL) {
        return;
    }

    now = time(NULL);
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tmv) == 0) {
        snprintf(timestamp, sizeof(timestamp), "0000-00-00 00:00:00");
    }

    fprintf(fp,
            "[%s] actor=%s action=%s target=%s details=%s\n",
            timestamp,
            actor_name,
            action_name,
            target_name,
            detail_text);
    fclose(fp);
}

#define MODLOG_SCAN_BUF  16384
#define MODLOG_MAX_LINES 200

/* Forward declaration — defined later in this file */
static void send_system_chat_message(socket_handle_t socket_fd,
                                     const char *message,
                                     int overlay);

static void send_modlog_to_player(socket_handle_t socket_fd, int count) {
    FILE   *fp;
    long    file_size, seek_pos, actual_read;
    char   *buf;
    char   *all_lines[MODLOG_MAX_LINES];
    int     nlines     = 0;
    char   *p, *line_start;
    int     send_from, i;
    char    msg[300];

    if (count < 1)  count = 10;
    if (count > 20) count = 20;

    fp = fopen(MOD_AUDIT_LOG_FILE_PATH, "rb");
    if (fp == NULL) {
        send_system_chat_message(socket_fd, "[MOD] No audit log found.", 0);
        return;
    }

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        send_system_chat_message(socket_fd, "[MOD] Audit log is empty.", 0);
        return;
    }

    seek_pos    = (file_size > MODLOG_SCAN_BUF) ? (file_size - MODLOG_SCAN_BUF) : 0;
    actual_read = file_size - seek_pos;
    fseek(fp, seek_pos, SEEK_SET);

    buf = (char *)malloc((size_t)(actual_read + 1));
    if (buf == NULL) {
        fclose(fp);
        send_system_chat_message(socket_fd, "[MOD] Memory error.", 0);
        return;
    }
    actual_read = (long)fread(buf, 1, (size_t)actual_read, fp);
    fclose(fp);
    buf[actual_read] = '\0';

    /* Null-terminate each line and collect pointers */
    p         = buf;
    line_start = buf;
    while (*p != '\0') {
        if (*p == '\n') {
            *p = '\0';
            if (p > buf && *(p - 1) == '\r') *(p - 1) = '\0';
            if (*line_start != '\0' && nlines < MODLOG_MAX_LINES) {
                all_lines[nlines++] = line_start;
            }
            line_start = p + 1;
        }
        ++p;
    }
    /* Last line without trailing newline */
    if (*line_start != '\0' && nlines < MODLOG_MAX_LINES) {
        all_lines[nlines++] = line_start;
    }

    if (nlines == 0) {
        free(buf);
        send_system_chat_message(socket_fd, "[MOD] Audit log is empty.", 0);
        return;
    }

    send_from = nlines - count;
    if (send_from < 0) send_from = 0;

    snprintf(msg, sizeof(msg), "[MOD] Last %d audit log %s:",
             nlines - send_from, (nlines - send_from == 1) ? "entry" : "entries");
    send_system_chat_message(socket_fd, msg, 0);

    for (i = send_from; i < nlines; ++i) {
        snprintf(msg, sizeof(msg), "%s", all_lines[i]);
        send_system_chat_message(socket_fd, msg, 0);
    }
    free(buf);
}

static int record_chat_message_and_check_limit(client_session_t *session,
                                               time_t now,
                                               int *seconds_left) {
    int block_seconds;
    char audit_detail[96];

    if (session == NULL) {
        return 1;
    }

    if (seconds_left != NULL) {
        *seconds_left = 0;
    }

    if (session->chat_rate_block_until > now) {
        if (seconds_left != NULL) {
            *seconds_left = (int)(session->chat_rate_block_until - now);
        }
        return 0;
    }

    if (session->chat_rate_window_start == 0 ||
        (now - session->chat_rate_window_start) >= CHAT_RATE_WINDOW_SECONDS) {
        session->chat_rate_window_start = now;
        session->chat_rate_message_count = 0;
    }

    session->chat_rate_message_count += 1;
    if (session->chat_rate_message_count > CHAT_RATE_MAX_MESSAGES) {
        if (session->chat_rate_last_violation == 0 ||
            (now - session->chat_rate_last_violation) >= CHAT_RATE_STRIKE_DECAY_SECONDS) {
            session->chat_rate_strike_count = 0;
        }

        if (session->chat_rate_strike_count < 30) {
            session->chat_rate_strike_count += 1;
        }

        block_seconds = CHAT_RATE_BLOCK_BASE_SECONDS;
        if (session->chat_rate_strike_count > 1) {
            int shift = session->chat_rate_strike_count - 1;
            if (shift > 6) {
                shift = 6;
            }
            block_seconds <<= shift;
        }
        if (block_seconds > CHAT_RATE_BLOCK_MAX_SECONDS) {
            block_seconds = CHAT_RATE_BLOCK_MAX_SECONDS;
        }

        session->chat_rate_last_violation = now;
        session->chat_rate_block_until = now + block_seconds;
        session->chat_rate_window_start = now;
        session->chat_rate_message_count = 0;
        if (seconds_left != NULL) {
            *seconds_left = block_seconds;
        }

        snprintf(audit_detail,
                 sizeof(audit_detail),
                 "temporary mute %ds strike=%d",
                 block_seconds,
                 session->chat_rate_strike_count);
        append_moderation_audit(session->username,
                                "CHAT_RATE_LIMIT",
                                "-",
                                audit_detail);
        return 0;
    }

    return 1;
}

static void register_play_chat_socket(socket_handle_t socket_fd, const char *username) {
    size_t i;

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_play_chat_socket_count; ++i) {
        if (g_play_chat_sockets[i] == socket_fd) {
            snprintf(g_play_chat_socket_usernames[i],
                     sizeof(g_play_chat_socket_usernames[i]),
                     "%s",
                     (username != NULL && username[0] != '\0') ? username : "Player");
            PLAY_CHAT_UNLOCK();
            return;
        }
    }

    if (g_play_chat_socket_count < MAX_PLAY_CHAT_SOCKETS) {
        size_t idx = g_play_chat_socket_count;
        g_play_chat_sockets[idx] = socket_fd;
        snprintf(g_play_chat_socket_usernames[idx],
                 sizeof(g_play_chat_socket_usernames[idx]),
                 "%s",
                 (username != NULL && username[0] != '\0') ? username : "Player");
        g_play_chat_socket_count += 1;
    }
    PLAY_CHAT_UNLOCK();
}

static void unregister_play_chat_socket(socket_handle_t socket_fd) {
    size_t i;

    PLAY_CHAT_LOCK();
    for (i = 0; i < g_play_chat_socket_count; ++i) {
        if (g_play_chat_sockets[i] == socket_fd) {
            size_t tail = g_play_chat_socket_count - i - 1;
            if (tail > 0) {
                memmove(&g_play_chat_sockets[i],
                        &g_play_chat_sockets[i + 1],
                        tail * sizeof(g_play_chat_sockets[0]));
                memmove(&g_play_chat_socket_usernames[i],
                        &g_play_chat_socket_usernames[i + 1],
                        tail * sizeof(g_play_chat_socket_usernames[0]));
            }
            g_play_chat_socket_count -= 1;
            break;
        }
    }
    PLAY_CHAT_UNLOCK();
}

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

static const char *entity_event_kind_name(int event_kind) {
    switch (event_kind) {
        case ENTITY_EVENT_SPAWN:
            return "spawn";
        case ENTITY_EVENT_UPDATE:
            return "update";
        case ENTITY_EVENT_REMOVE:
            return "remove";
        default:
            return "unknown";
    }
}

static void log_and_clear_entity_events(entity_manager_t *manager) {
    if (manager == NULL || manager->pending_count == 0) {
        return;
    }

    printf("Entity event dump: count=%zu, dropped=%zu\n", manager->pending_count, manager->dropped_events);
    for (size_t i = 0; i < manager->pending_count; ++i) {
        const entity_event_t *evt = &manager->pending[i];
        printf("  [%zu] kind=%s id=%d entity_kind=%d pos=(%.3f, %.3f, %.3f)\n",
               i,
               entity_event_kind_name(evt->event_kind),
               evt->entity_id,
               evt->entity_kind,
               evt->x,
               evt->y,
               evt->z);
    }

    entity_manager_clear(manager);
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
    int32_t packet_len = -1;
    int32_t data_len = -1;

    if (buf == NULL || out_packet_id == NULL || out_payload == NULL || out_payload_len == NULL ||
        inflate_buf == NULL || inflate_buf_size == 0) {
        return 0;
    }

    packet_len = read_varint(&p, &len);
    if (packet_len < 0) {
        return 0;
    }

    if ((size_t)packet_len > len) {
        return 0;
    }

    len = (size_t)packet_len;
    if (len == 0) {
        return 0;
    }

    data_len = read_varint(&p, &len);
    if (data_len < 0) {
        return 0;
    }

    if (data_len == 0) {
        const uint8_t *payload_ptr = p;
        size_t payload_len = len;
        *out_packet_id = read_varint(&payload_ptr, &payload_len);
        if (*out_packet_id < 0) {
            return 0;
        }
        *out_payload = payload_ptr;
        *out_payload_len = payload_len;
        return 1;
    }

    {
        uLongf inflated_len = (uLongf)inflate_buf_size;
        if ((size_t)data_len > inflate_buf_size) {
            return 0;
        }
        int zres = uncompress(inflate_buf, &inflated_len, p, (uLong)len);
        if (zres != Z_OK || inflated_len == 0) {
            return 0;
        }

        if ((int32_t)inflated_len != data_len) {
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
                                                 double *out_y,
                                                 double *out_z) {
    /*
     * Accept common serverbound movement packet IDs used across nearby protocol
     * revisions. Position packets always start with x,y,z doubles.
     */
    int is_position_packet =
        // protocol 774 (1.21.11)
        (packet_id == 0x1D) || (packet_id == 0x1E) ||
        // nearby protocol revisions / fallback compatibility
        (packet_id == 0x1A) || (packet_id == 0x1B) ||
        (packet_id == 0x15) || (packet_id == 0x16);

    if (!is_position_packet || payload == NULL || payload_len < 24 || out_x == NULL || out_y == NULL || out_z == NULL) {
        return 0;
    }

    *out_x = read_be_double_ptr(payload);
    *out_y = read_be_double_ptr(payload + 8);
    *out_z = read_be_double_ptr(payload + 16);
    return 1;
}

static void decode_packed_block_position(uint64_t packed,
                                         int32_t *out_x,
                                         int32_t *out_y,
                                         int32_t *out_z) {
    int32_t x = (int32_t)((packed >> 38) & 0x3FFFFFFu);
    int32_t y = (int32_t)(packed & 0xFFFu);
    int32_t z = (int32_t)((packed >> 12) & 0x3FFFFFFu);

    if (x >= (1 << 25)) x -= (1 << 26);
    if (y >= (1 << 11)) y -= (1 << 12);
    if (z >= (1 << 25)) z -= (1 << 26);

    if (out_x != NULL) *out_x = x;
    if (out_y != NULL) *out_y = y;
    if (out_z != NULL) *out_z = z;
}

static int try_parse_block_dig_packet_774(const uint8_t *payload,
                                          size_t payload_len,
                                          int32_t *out_status,
                                          int32_t *out_x,
                                          int32_t *out_y,
                                          int32_t *out_z,
                                          int32_t *out_sequence) {
    const uint8_t *p = payload;
    size_t len = payload_len;
    int32_t status;
    uint64_t packed_pos;

    if (payload == NULL || out_status == NULL || out_x == NULL || out_y == NULL || out_z == NULL || out_sequence == NULL) {
        return 0;
    }

    status = read_varint(&p, &len);
    if (status < 0 || len < 10) {
        return 0;
    }

    packed_pos =
        ((uint64_t)p[0] << 56) |
        ((uint64_t)p[1] << 48) |
        ((uint64_t)p[2] << 40) |
        ((uint64_t)p[3] << 32) |
        ((uint64_t)p[4] << 24) |
        ((uint64_t)p[5] << 16) |
        ((uint64_t)p[6] << 8) |
        (uint64_t)p[7];
    p += 8;
    len -= 8;

    /* face (i8) */
    p += 1;
    len -= 1;

    /* sequence (varint) */
    {
        int32_t sequence = read_varint(&p, &len);
        if (sequence < 0) {
            return 0;
        }
        *out_sequence = sequence;
    }

    *out_status = status;
    decode_packed_block_position(packed_pos, out_x, out_y, out_z);
    return 1;
}

static int read_prefixed_string_copy(const uint8_t **payload,
                                     size_t *payload_len,
                                     char *out,
                                     size_t out_size) {
    int32_t str_len;
    size_t copy_len;

    if (payload == NULL || *payload == NULL || payload_len == NULL || out == NULL || out_size == 0) {
        return 0;
    }

    str_len = read_varint(payload, payload_len);
    if (str_len < 0 || (size_t)str_len > *payload_len) {
        return 0;
    }

    copy_len = (size_t)str_len;
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }

    memcpy(out, *payload, copy_len);
    out[copy_len] = '\0';

    *payload += (size_t)str_len;
    *payload_len -= (size_t)str_len;
    return 1;
}

static int try_parse_spawn_request_ex(const char *text,
                                      char *out_entity_name,
                                      size_t out_entity_name_size,
                                      int *out_count,
                                      double *out_radius,
                                      double *out_y_offset,
                                      int *out_glow) {
    const char *p = text;
    const char *start;
    const char *end;
    size_t len;
    int count = 1;
    double radius = 2.0;
    double y_offset = 0.0;
    int glow = 0;

    if (text == NULL || out_entity_name == NULL || out_entity_name_size == 0 || out_count == NULL ||
        out_radius == NULL || out_y_offset == NULL || out_glow == NULL) {
        return 0;
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '/') {
        ++p;
    }

    if (strncmp(p, "spawn", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "summon", 6) == 0) {
        p += 6;
    } else {
        return 0;
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0') {
        return 0;
    }

    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        ++p;
    }
    end = p;

    len = (size_t)(end - start);
    if (len == 0) {
        return 0;
    }
    if (len >= out_entity_name_size) {
        len = out_entity_name_size - 1;
    }
    memcpy(out_entity_name, start, len);
    out_entity_name[len] = '\0';

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p != '\0') {
        char *parse_end = NULL;
        long parsed = strtol(p, &parse_end, 10);
        if (parse_end != p && parsed > 0 && parsed <= 32) {
            count = (int)parsed;
            p = parse_end;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
        }
    }

    if (*p != '\0') {
        char *parse_end = NULL;
        double parsed = strtod(p, &parse_end);
        if (parse_end != p && parsed >= 0.0 && parsed <= 32.0) {
            radius = parsed;
            p = parse_end;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
        }
    }

    if (*p != '\0') {
        char *parse_end = NULL;
        double parsed = strtod(p, &parse_end);
        if (parse_end != p && parsed >= -32.0 && parsed <= 32.0) {
            y_offset = parsed;
            p = parse_end;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
        }
    }

    if (*p != '\0') {
        if (strcmp(p, "glow") == 0) {
            glow = 1;
        } else if (strcmp(p, "noglow") == 0) {
            glow = 0;
        }
    }

    *out_count = count;
    *out_radius = radius;
    *out_y_offset = y_offset;
    *out_glow = glow;
    return 1;
}

static int command_equals(const char *text, const char *cmd) {
    const char *p;
    size_t cmd_len;

    if (text == NULL || cmd == NULL) {
        return 0;
    }

    p = text;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '/') {
        ++p;
    }

    cmd_len = strlen(cmd);
    if (strncmp(p, cmd, cmd_len) != 0) {
        return 0;
    }

    return p[cmd_len] == '\0' || p[cmd_len] == ' ' || p[cmd_len] == '\t';
}

static int command_extract_single_arg(const char *text,
                                      const char *cmd,
                                      char *out_arg,
                                      size_t out_arg_size) {
    const char *p;
    const char *start;
    size_t cmd_len;
    size_t len;

    if (text == NULL || cmd == NULL || out_arg == NULL || out_arg_size == 0) {
        return 0;
    }

    p = text;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '/') {
        ++p;
    }

    cmd_len = strlen(cmd);
    if (strncmp(p, cmd, cmd_len) != 0) {
        return 0;
    }

    p += cmd_len;
    if (*p != ' ' && *p != '\t') {
        return 0;
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0') {
        return 0;
    }

    start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        ++p;
    }

    len = (size_t)(p - start);
    if (len == 0) {
        return 0;
    }

    if (len >= out_arg_size) {
        len = out_arg_size - 1;
    }

    memcpy(out_arg, start, len);
    out_arg[len] = '\0';
    return 1;
}

static int find_entity_position(const entity_registry_t *registry,
                                int32_t entity_id,
                                double *out_x,
                                double *out_y,
                                double *out_z) {
    int i;

    if (registry == NULL || out_x == NULL || out_y == NULL || out_z == NULL) {
        return 0;
    }

    for (i = 0; i < 512; ++i) {
        const entity_state_t *e = &registry->entries[i];
        if (e->active && e->entity_id == entity_id) {
            *out_x = e->x;
            *out_y = e->y;
            *out_z = e->z;
            return 1;
        }
    }

    return 0;
}

static int try_parse_despawn_request(const char *text,
                                     int *out_all,
                                     int32_t *out_entity_id) {
    const char *p = text;

    if (text == NULL || out_all == NULL || out_entity_id == NULL) {
        return 0;
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '/') {
        ++p;
    }

    if (strncmp(p, "despawnall", 10) == 0 &&
        (p[10] == '\0' || p[10] == ' ' || p[10] == '\t')) {
        *out_all = 1;
        *out_entity_id = 0;
        return 1;
    }

    if (strncmp(p, "despawn", 7) != 0 ||
        (p[7] != '\0' && p[7] != ' ' && p[7] != '\t')) {
        return 0;
    }

    p += 7;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }

    if (*p == '\0') {
        return 0;
    }

    {
        char *parse_end = NULL;
        long parsed = strtol(p, &parse_end, 10);
        if (parse_end == p || parsed <= 0 || parsed > 2147483647L) {
            return 0;
        }
        *out_all = 0;
        *out_entity_id = (int32_t)parsed;
        return 1;
    }
}

static int lookup_entity_type_774(const char *entity_name, int32_t *out_entity_type) {
    struct entity_type_map_entry {
        const char *name;
        int32_t type_id;
    };
    static const struct entity_type_map_entry map[] = {
        {"armor_stand", 5},
        {"chicken", 26},
        {"cow", 30},
        {"sheep", 111},
        {"pig", 100},
        {"rabbit", 108},
        {"wolf", 148},
        {"cat", 21},
        {"horse", 66},
        {"villager", 139},
        {"zombie", 150},
        {"skeleton", 115},
        {"creeper", 32},
        {"spider", 124},
        {"enderman", 41}
    };
    char normalized[64];
    size_t i;
    size_t n;

    if (entity_name == NULL || out_entity_type == NULL) {
        return 0;
    }

    n = strlen(entity_name);
    if (n == 0 || n >= sizeof(normalized)) {
        return 0;
    }

    for (i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)entity_name[i];
        if (ch == '-') {
            normalized[i] = '_';
        } else {
            normalized[i] = (char)tolower(ch);
        }
    }
    normalized[n] = '\0';

    for (i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcmp(normalized, map[i].name) == 0) {
            *out_entity_type = map[i].type_id;
            return 1;
        }
    }

    return 0;
}

static const char *lookup_entity_name_by_type_774(int32_t entity_type) {
    switch (entity_type) {
        case 5: return "armor_stand";
        case 21: return "cat";
        case 26: return "chicken";
        case 30: return "cow";
        case 32: return "creeper";
        case 41: return "enderman";
        case 66: return "horse";
        case 100: return "pig";
        case 108: return "rabbit";
        case 111: return "sheep";
        case 115: return "skeleton";
        case 124: return "spider";
        case 139: return "villager";
        case 148: return "wolf";
        case 150: return "zombie";
        default: return "unknown";
    }
}

static int32_t pick_roaming_mob_type(size_t index) {
    static const int32_t types[] = { 30, 100, 111, 150, 115, 32, 124, 41 };
    return types[index % (sizeof(types) / sizeof(types[0]))];
}

static double resolve_generated_ground_y(double x, double z) {
    int32_t block_x = (int32_t)floor(x);
    int32_t block_z = (int32_t)floor(z);
    return (double)generated_world_surface_y(block_x, block_z) + 1.0;
}

static int send_stream_chunk(client_session_t *session,
                             int32_t chunk_x,
                             int32_t chunk_z,
                             const world_info_t *world_info,
                             chunk_send_result_t *out_result) {
    int sent_this = 0;
    int world_source_mode;

    if (out_result != NULL) {
        *out_result = CHUNK_SEND_RESULT_NONE;
    }

    world_source_mode = session->server_config.world_source_mode;
    if (session->stream_state.force_debug_spawn) {
        world_source_mode = WORLD_SOURCE_MODE_DEBUG;
    }

    if ((world_source_mode == WORLD_SOURCE_MODE_AUTO || world_source_mode == WORLD_SOURCE_MODE_REAL) &&
        !session->stream_state.force_debug_spawn &&
        session->server_config.enable_real_chunks &&
        session->stream_state.has_world_info) {
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
                send_large_post_compression_packet(session->socket_fd, real_buf, real_len);
                if (g_log_chunk_sends) {
                    printf("Sent REAL chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, real_len);
                }
                free(real_buf);
                sent_this = 1;
                if (out_result != NULL) {
                    *out_result = CHUNK_SEND_RESULT_REAL;
                }
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
                    send_large_post_compression_packet(session->socket_fd, real_buf, real_len);
                    if (g_log_chunk_sends) {
                        printf("Sent REAL chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, real_len);
                    }
                    free(real_buf);
                    sent_this = 1;
                    if (out_result != NULL) {
                        *out_result = CHUNK_SEND_RESULT_REAL;
                    }
                }
                free(chunk_nbt);
            }
        }
    }

    if (!sent_this && (world_source_mode == WORLD_SOURCE_MODE_AUTO || world_source_mode == WORLD_SOURCE_MODE_GENERATED)) {
        uint8_t *cached_packet = NULL;
        size_t cached_packet_len = 0;

        if (load_cached_generated_chunk_packet(&session->server_config,
                                               chunk_x,
                                               chunk_z,
                                               &cached_packet,
                                               &cached_packet_len)) {
            char world_root[1024];
            send_large_post_compression_packet(session->socket_fd, cached_packet, cached_packet_len);
            if (g_log_chunk_sends) {
                printf("Sent CACHED generated chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, cached_packet_len);
            }
            if (resolve_generated_world_root(&session->server_config, world_root, sizeof(world_root))) {
                (void)ensure_generated_world_region_chunk_stub(world_root, chunk_x, chunk_z);
            }
            free(cached_packet);
            sent_this = 1;
            if (out_result != NULL) {
                *out_result = CHUNK_SEND_RESULT_GENERATED;
            }
        }
    }

    if (!sent_this && (world_source_mode == WORLD_SOURCE_MODE_AUTO || world_source_mode == WORLD_SOURCE_MODE_GENERATED)) {
        size_t gen_len = 0;
        fprintf(stderr, "[CHUNK_SEND] Calling build_generated_overworld_chunk_packet for (%d,%d)\n", chunk_x, chunk_z);
        fflush(stderr);
        
        FILE *gen_log = fopen("chunk_generate.log", "a");
        if (gen_log) {
            fprintf(gen_log, "[CHUNK_SEND] START generating chunk (%d,%d)\n", chunk_x, chunk_z);
            fflush(gen_log);
            fclose(gen_log);
        }
        
        uint8_t *gen_buf = build_generated_overworld_chunk_packet(chunk_x, chunk_z, &gen_len);
        
        FILE *gen_log2 = fopen("chunk_generate.log", "a");
        if (gen_log2) {
            fprintf(gen_log2, "[CHUNK_SEND] END generating chunk (%d,%d), result=%s\n", chunk_x, chunk_z, gen_buf ? "SUCCESS" : "NULL");
            fflush(gen_log2);
            fclose(gen_log2);
        }
        
        if (gen_buf == NULL) {
            uint8_t *chunk_nbt = NULL;
            size_t chunk_nbt_len = 0;
            if (build_generated_chunk_nbt_payload(chunk_x, chunk_z, &chunk_nbt, &chunk_nbt_len)) {
                gen_buf = build_chunk_data_packet(chunk_nbt,
                                                  chunk_nbt_len,
                                                  chunk_x,
                                                  chunk_z,
                                                  &gen_len);
                free(chunk_nbt);
            }
        }
        if (gen_buf == NULL) {
            gen_buf = build_generated_overworld_chunk_packet(chunk_x, chunk_z, &gen_len);
            if (gen_buf != NULL) {
                printf("WARNING: canonical generated chunk serialization failed for (%d,%d); using direct generator fallback.\n",
                       chunk_x,
                       chunk_z);
            }
        }
        if (gen_buf) {
            send_large_post_compression_packet(session->socket_fd, gen_buf, gen_len);
            if (g_log_chunk_sends) {
                printf("Sent GENERATED chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, gen_len);
            }
            save_cached_generated_chunk_packet(&session->server_config,
                                               chunk_x,
                                               chunk_z,
                                               gen_buf,
                                               gen_len);
            free(gen_buf);
            sent_this = 1;
            if (out_result != NULL) {
                *out_result = CHUNK_SEND_RESULT_GENERATED;
            }
        }
    }

    if (!sent_this &&
        ((world_source_mode == WORLD_SOURCE_MODE_AUTO &&
          (session->stream_state.force_debug_spawn ||
           !session->server_config.enable_real_chunks ||
           session->server_config.allow_debug_chunk_fallback)) ||
         world_source_mode == WORLD_SOURCE_MODE_DEBUG)) {
        size_t dbg_len = 0;
        uint8_t *dbg_buf = build_debug_flat_chunk_packet(chunk_x, chunk_z, &dbg_len);
        if (dbg_buf) {
            send_large_post_compression_packet(session->socket_fd, dbg_buf, dbg_len);
            if (g_log_chunk_sends) {
                printf("Sent DEBUG flat chunk for (%d,%d), %zu bytes.\n", chunk_x, chunk_z, dbg_len);
            }
            free(dbg_buf);
            sent_this = 1;
            if (out_result != NULL) {
                *out_result = CHUNK_SEND_RESULT_DEBUG;
            }
        }
    }

    if (sent_this) {
        replay_broken_blocks_for_chunk(session, chunk_x, chunk_z);
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

static int resolve_chunk_stream_radius_for_world_source(const server_config_t *server_config,
                                                        int resolved_world_source_mode) {
    int radius = resolve_chunk_stream_radius(server_config);

    if (resolved_world_source_mode == WORLD_SOURCE_MODE_GENERATED && radius > 2) {
        radius = 2;
    }
    if (resolved_world_source_mode == WORLD_SOURCE_MODE_DEBUG && radius > 3) {
        radius = 3;
    }

    return radius;
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

static void path_dirname_inplace(char *path) {
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return;
    }

    len = strlen(path);
    while (len > 0) {
        char ch = path[len - 1];
        if (ch == '/' || ch == '\\') {
            path[len - 1] = '\0';
            return;
        }
        len -= 1;
    }

    path[0] = '\0';
}

static int get_executable_dir_main(char *buffer, size_t buffer_size) {
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)buffer_size);
    if (length == 0 || length >= buffer_size) {
        return 0;
    }
    path_dirname_inplace(buffer);
    return buffer[0] != '\0';
#else
    ssize_t length = readlink("/proc/self/exe", buffer, buffer_size - 1);
    if (length <= 0 || (size_t)length >= buffer_size) {
        return 0;
    }
    buffer[length] = '\0';
    path_dirname_inplace(buffer);
    return buffer[0] != '\0';
#endif
}

static int directory_exists_at(const char *path) {
    struct stat st;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (stat(path, &st) != 0) {
        return 0;
    }
#ifdef _WIN32
    return (st.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(st.st_mode);
#endif
}

static int create_dir_if_missing(const char *path) {
    if (directory_exists_at(path)) {
        return 1;
    }
#ifdef _WIN32
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

static void write_be16_ptr(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFFu);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void write_be32_ptr(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static void write_be64_ptr(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)((v >> 56) & 0xFFu);
    p[1] = (uint8_t)((v >> 48) & 0xFFu);
    p[2] = (uint8_t)((v >> 40) & 0xFFu);
    p[3] = (uint8_t)((v >> 32) & 0xFFu);
    p[4] = (uint8_t)((v >> 24) & 0xFFu);
    p[5] = (uint8_t)((v >> 16) & 0xFFu);
    p[6] = (uint8_t)((v >> 8) & 0xFFu);
    p[7] = (uint8_t)(v & 0xFFu);
}

static int gzip_write_file(const char *path, const uint8_t *raw, size_t raw_len) {
    z_stream stream;
    uint8_t *compressed;
    size_t compressed_cap;
    size_t compressed_len;
    FILE *fp;

    if (path == NULL || raw == NULL || raw_len == 0) {
        return 0;
    }

    compressed_cap = raw_len + (raw_len / 8u) + 128u;
    compressed = (uint8_t *)malloc(compressed_cap);
    if (compressed == NULL) {
        return 0;
    }

    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)raw;
    stream.avail_in = (uInt)raw_len;
    stream.next_out = compressed;
    stream.avail_out = (uInt)compressed_cap;

    if (deflateInit2(&stream,
                     Z_DEFAULT_COMPRESSION,
                     Z_DEFLATED,
                     15 + 16,
                     8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        free(compressed);
        return 0;
    }

    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&stream);
        free(compressed);
        return 0;
    }

    compressed_len = (size_t)stream.total_out;
    deflateEnd(&stream);

    fp = fopen(path, "wb");
    if (fp == NULL) {
        free(compressed);
        return 0;
    }

    if (fwrite(compressed, 1, compressed_len, fp) != compressed_len) {
        fclose(fp);
        free(compressed);
        return 0;
    }

    fclose(fp);
    free(compressed);
    return 1;
}

static int zlib_compress_buffer(const uint8_t *raw,
                                size_t raw_len,
                                uint8_t **out_data,
                                size_t *out_len) {
    z_stream stream;
    uint8_t *compressed;
    size_t compressed_cap;

    if (raw == NULL || raw_len == 0 || out_data == NULL || out_len == NULL) {
        return 0;
    }

    *out_data = NULL;
    *out_len = 0;
    compressed_cap = raw_len + (raw_len / 8u) + 128u;
    compressed = (uint8_t *)malloc(compressed_cap);
    if (compressed == NULL) {
        return 0;
    }

    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)raw;
    stream.avail_in = (uInt)raw_len;
    stream.next_out = compressed;
    stream.avail_out = (uInt)compressed_cap;

    if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
        free(compressed);
        return 0;
    }

    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&stream);
        free(compressed);
        return 0;
    }

    *out_len = (size_t)stream.total_out;
    deflateEnd(&stream);
    *out_data = compressed;
    return 1;
}

typedef enum {
    GEN_BLOCK_AIR = 0,
    GEN_BLOCK_STONE = 1,
    GEN_BLOCK_GRASS = 2,
    GEN_BLOCK_DIRT = 3,
    GEN_BLOCK_WATER = 4,
    GEN_BLOCK_SAND = 5,
    GEN_BLOCK_OAK_LOG = 6,
    GEN_BLOCK_OAK_LEAVES = 7
} gen_block_kind_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} nbt_buf_t;

static int nbt_buf_grow(nbt_buf_t *b, size_t extra) {
    uint8_t *grown;
    size_t next_cap;

    if (b->len + extra <= b->cap) {
        return 1;
    }
    next_cap = b->cap ? b->cap : 4096;
    while (next_cap < b->len + extra) {
        next_cap *= 2;
    }
    grown = (uint8_t *)realloc(b->data, next_cap);
    if (grown == NULL) {
        return 0;
    }
    b->data = grown;
    b->cap = next_cap;
    return 1;
}

static int nbt_buf_write(nbt_buf_t *b, const void *src, size_t n) {
    if (!nbt_buf_grow(b, n)) {
        return 0;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 1;
}

static int nbt_u8(nbt_buf_t *b, uint8_t v) {
    return nbt_buf_write(b, &v, 1);
}

static int nbt_be16(nbt_buf_t *b, uint16_t v) {
    uint8_t t[2];
    write_be16_ptr(t, v);
    return nbt_buf_write(b, t, sizeof(t));
}

static int nbt_be32(nbt_buf_t *b, uint32_t v) {
    uint8_t t[4];
    write_be32_ptr(t, v);
    return nbt_buf_write(b, t, sizeof(t));
}

static int nbt_be64(nbt_buf_t *b, uint64_t v) {
    uint8_t t[8];
    write_be64_ptr(t, v);
    return nbt_buf_write(b, t, sizeof(t));
}

static int nbt_name(nbt_buf_t *b, const char *name) {
    size_t n = strlen(name);
    if (n > 65535u) {
        return 0;
    }
    return nbt_be16(b, (uint16_t)n) && nbt_buf_write(b, name, n);
}

static int nbt_tag_header(nbt_buf_t *b, uint8_t type, const char *name) {
    return nbt_u8(b, type) && nbt_name(b, name);
}

static int nbt_tag_int(nbt_buf_t *b, const char *name, int32_t v) {
    return nbt_tag_header(b, 3, name) && nbt_be32(b, (uint32_t)v);
}

static int nbt_tag_byte(nbt_buf_t *b, const char *name, int8_t v) {
    return nbt_tag_header(b, 1, name) && nbt_u8(b, (uint8_t)v);
}

static int nbt_tag_string(nbt_buf_t *b, const char *name, const char *value) {
    size_t n = strlen(value);
    if (n > 65535u) {
        return 0;
    }
    return nbt_tag_header(b, 8, name) && nbt_be16(b, (uint16_t)n) && nbt_buf_write(b, value, n);
}

static int nbt_tag_long_array(nbt_buf_t *b,
                              const char *name,
                              const uint64_t *arr,
                              uint32_t count) {
    if (!nbt_tag_header(b, 12, name) || !nbt_be32(b, count)) {
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!nbt_be64(b, arr[i])) {
            return 0;
        }
    }
    return 1;
}

static uint32_t generated_world_hash_persist(int32_t block_x, int32_t block_z) {
    uint32_t x = (uint32_t)block_x;
    uint32_t z = (uint32_t)block_z;
    uint32_t hash = x * 374761393u;

    hash += z * 668265263u;
    hash = (hash ^ (hash >> 13)) * 1274126177u;
    return hash ^ (hash >> 16);
}

static int generated_world_chunk_local_persist(int32_t value) {
    int local = value % 16;
    if (local < 0) {
        local += 16;
    }
    return local;
}

static int generated_world_has_tree_persist(int32_t block_x, int32_t block_z, int32_t surface_y) {
    int32_t slope_x;
    int32_t slope_z;
    int local_x = generated_world_chunk_local_persist(block_x);
    int local_z = generated_world_chunk_local_persist(block_z);
    int chance_mod = 64;
    double moisture = sin((double)block_x * 0.0075) + cos((double)block_z * 0.0065);

    if (surface_y <= 65 || surface_y >= 100) {
        return 0;
    }
    if (local_x < 1 || local_x > 14 || local_z < 1 || local_z > 14) {
        return 0;
    }

    slope_x = abs(surface_y - generated_world_surface_y(block_x + 1, block_z));
    slope_x += abs(surface_y - generated_world_surface_y(block_x - 1, block_z));
    slope_z = abs(surface_y - generated_world_surface_y(block_x, block_z + 1));
    slope_z += abs(surface_y - generated_world_surface_y(block_x, block_z - 1));
    if (slope_x + slope_z > 3) {
        return 0;
    }

    if (moisture > 0.8) {
        chance_mod = 34;
    } else if (moisture > 0.2) {
        chance_mod = 46;
    } else if (moisture < -0.6) {
        chance_mod = 92;
    }
    if (surface_y > 86) {
        chance_mod += 24;
    }

    return (generated_world_hash_persist(block_x, block_z) % (uint32_t)chance_mod) == 0u;
}

static int generated_world_tree_height_persist(int32_t block_x, int32_t block_z) {
    return 4 + (int)((generated_world_hash_persist(block_x, block_z) >> 8) % 3u);
}

static gen_block_kind_t generated_world_tree_block_persist(int32_t world_x,
                                                           int32_t world_y,
                                                           int32_t world_z) {
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            int32_t trunk_x = world_x - dx;
            int32_t trunk_z = world_z - dz;
            int32_t trunk_surface_y = generated_world_surface_y(trunk_x, trunk_z);
            int trunk_height;
            int crown_base_y;
            int leaf_radius;
            int leaf_dx;
            int leaf_dz;
            int leaf_dy;

            if (!generated_world_has_tree_persist(trunk_x, trunk_z, trunk_surface_y)) {
                continue;
            }

            trunk_height = generated_world_tree_height_persist(trunk_x, trunk_z);
            if (world_x == trunk_x && world_z == trunk_z &&
                world_y > trunk_surface_y && world_y <= trunk_surface_y + trunk_height) {
                return GEN_BLOCK_OAK_LOG;
            }

            crown_base_y = trunk_surface_y + trunk_height - 2;
            leaf_dy = (int)(world_y - crown_base_y);
            if (leaf_dy < 0 || leaf_dy > 3) {
                continue;
            }

            leaf_radius = (leaf_dy == 3) ? 1 : 2;
            leaf_dx = abs(dx);
            leaf_dz = abs(dz);
            if (leaf_dx > leaf_radius || leaf_dz > leaf_radius) {
                continue;
            }
            if (leaf_dy == 3 && leaf_dx + leaf_dz > 1) {
                continue;
            }
            if (leaf_dy == 0 && leaf_dx == 2 && leaf_dz == 2) {
                continue;
            }
            if (world_x == trunk_x && world_z == trunk_z &&
                world_y <= trunk_surface_y + trunk_height) {
                continue;
            }

            return GEN_BLOCK_OAK_LEAVES;
        }
    }

    return GEN_BLOCK_AIR;
}

static gen_block_kind_t generated_world_block_kind(int32_t world_x, int32_t world_y, int32_t world_z) {
    int32_t surface_y = generated_world_surface_y(world_x, world_z);

    if (world_y <= surface_y) {
        if (world_y == surface_y) {
            return (surface_y <= 65) ? GEN_BLOCK_SAND : GEN_BLOCK_GRASS;
        }
        if (world_y >= surface_y - 3) {
            return (surface_y <= 67) ? GEN_BLOCK_SAND : GEN_BLOCK_DIRT;
        }
        return GEN_BLOCK_STONE;
    }

    if (world_y <= 64) {
        return GEN_BLOCK_WATER;
    }

    if (world_y <= surface_y + 8) {
        return generated_world_tree_block_persist(world_x, world_y, world_z);
    }

    return GEN_BLOCK_AIR;
}

static const char *generated_world_block_name(gen_block_kind_t k) {
    switch (k) {
        case GEN_BLOCK_STONE: return "minecraft:stone";
        case GEN_BLOCK_GRASS: return "minecraft:grass_block";
        case GEN_BLOCK_DIRT: return "minecraft:dirt";
        case GEN_BLOCK_WATER: return "minecraft:water";
        case GEN_BLOCK_SAND: return "minecraft:sand";
        case GEN_BLOCK_OAK_LOG: return "minecraft:oak_log";
        case GEN_BLOCK_OAK_LEAVES: return "minecraft:oak_leaves";
        case GEN_BLOCK_AIR:
        default:
            return "minecraft:air";
    }
}

static int generated_palette_index(gen_block_kind_t *palette,
                                   int *palette_size,
                                   gen_block_kind_t kind) {
    for (int i = 0; i < *palette_size; ++i) {
        if (palette[i] == kind) {
            return i;
        }
    }
    if (*palette_size >= 16) {
        return 0;
    }
    palette[*palette_size] = kind;
    *palette_size += 1;
    return *palette_size - 1;
}

static void pack_palette_indices_u64(const uint16_t *indices,
                                     int entry_count,
                                     int bits,
                                     uint64_t *out_longs,
                                     int long_count) {
    memset(out_longs, 0, (size_t)long_count * sizeof(uint64_t));
    for (int i = 0; i < entry_count; ++i) {
        uint32_t bit_index = (uint32_t)i * (uint32_t)bits;
        uint32_t long_index = bit_index >> 6;
        uint32_t start_bit = bit_index & 63u;
        uint64_t value = (uint64_t)indices[i];

        if ((int)long_index >= long_count) {
            break;
        }
        out_longs[long_index] |= value << start_bit;
        if (start_bit + (uint32_t)bits > 64u && (int)(long_index + 1u) < long_count) {
            out_longs[long_index + 1u] |= value >> (64u - start_bit);
        }
    }
}

static int build_generated_chunk_nbt_payload(int32_t chunk_x,
                                             int32_t chunk_z,
                                             uint8_t **out_nbt,
                                             size_t *out_nbt_len) {
    nbt_buf_t b = {0};
    uint64_t hm_longs[37];
    uint16_t hm_entries[256];
    const int min_world_y = -64;

    if (out_nbt == NULL || out_nbt_len == NULL) {
        return 0;
    }
    *out_nbt = NULL;
    *out_nbt_len = 0;

    memset(hm_entries, 0, sizeof(hm_entries));
    memset(hm_longs, 0, sizeof(hm_longs));

    for (int lz = 0; lz < 16; ++lz) {
        for (int lx = 0; lx < 16; ++lx) {
            int32_t wx = chunk_x * 16 + lx;
            int32_t wz = chunk_z * 16 + lz;
            int top_y = min_world_y - 1;
            for (int y = 319; y >= -64; --y) {
                if (generated_world_block_kind(wx, y, wz) != GEN_BLOCK_AIR) {
                    top_y = y;
                    break;
                }
            }
            {
                int idx = lz * 16 + lx;
                int hm_value = top_y + 1 - min_world_y;
                if (hm_value < 0) hm_value = 0;
                if (hm_value > 511) hm_value = 511;
                hm_entries[idx] = (uint16_t)hm_value;
            }
        }
    }

    {
        const int bits = 9;
        const int entries_per_long = 64 / bits;
        for (int i = 0; i < 256; ++i) {
            int long_index = i / entries_per_long;
            int bit_index = (i % entries_per_long) * bits;
            hm_longs[long_index] |= ((uint64_t)hm_entries[i]) << bit_index;
        }
    }

    if (!nbt_u8(&b, 10) || !nbt_be16(&b, 0)) goto fail; /* root compound */
    if (!nbt_tag_int(&b, "DataVersion", 3953)) goto fail;
    if (!nbt_tag_int(&b, "xPos", chunk_x)) goto fail;
    if (!nbt_tag_int(&b, "zPos", chunk_z)) goto fail;
    if (!nbt_tag_string(&b, "Status", "minecraft:full")) goto fail;

    if (!nbt_tag_header(&b, 10, "Heightmaps")) goto fail;
    if (!nbt_tag_long_array(&b, "WORLD_SURFACE", hm_longs, 37u)) goto fail;
    if (!nbt_u8(&b, 0)) goto fail;

    if (!nbt_tag_header(&b, 9, "sections") || !nbt_u8(&b, 10) || !nbt_be32(&b, 24u)) goto fail;
    for (int sy = -4; sy <= 19; ++sy) {
        gen_block_kind_t palette[16];
        int palette_size = 0;
        uint16_t indices[4096];
        int y_base = sy * 16;

        if (!nbt_u8(&b, 10)) goto fail; /* section compound */
        if (!nbt_tag_byte(&b, "Y", (int8_t)sy)) goto fail;

        for (int ly = 0; ly < 16; ++ly) {
            int wy = y_base + ly;
            for (int lz = 0; lz < 16; ++lz) {
                int wz = chunk_z * 16 + lz;
                for (int lx = 0; lx < 16; ++lx) {
                    int wx = chunk_x * 16 + lx;
                    gen_block_kind_t kind = generated_world_block_kind(wx, wy, wz);
                    int index = (ly << 8) | (lz << 4) | lx;
                    indices[index] = (uint16_t)generated_palette_index(palette, &palette_size, kind);
                }
            }
        }

        if (!nbt_tag_header(&b, 10, "block_states")) goto fail;
        if (!nbt_tag_header(&b, 9, "palette") || !nbt_u8(&b, 10) || !nbt_be32(&b, (uint32_t)palette_size)) goto fail;
        for (int i = 0; i < palette_size; ++i) {
            if (!nbt_u8(&b, 10)) goto fail;
            if (!nbt_tag_string(&b, "Name", generated_world_block_name(palette[i]))) goto fail;
            if (!nbt_u8(&b, 0)) goto fail;
        }

        if (palette_size > 1) {
            int bits = 0;
            int long_count;
            uint64_t longs[256];
            int ps = palette_size - 1;
            while (ps > 0) {
                bits += 1;
                ps >>= 1;
            }
            if (bits < 4) bits = 4;
            long_count = (int)((4096 * bits + 63) / 64);
            pack_palette_indices_u64(indices, 4096, bits, longs, long_count);
            if (!nbt_tag_long_array(&b, "data", longs, (uint32_t)long_count)) goto fail;
        }
        if (!nbt_u8(&b, 0)) goto fail; /* end block_states */

        if (!nbt_tag_header(&b, 10, "biomes")) goto fail;
        if (!nbt_tag_header(&b, 9, "palette") || !nbt_u8(&b, 8) || !nbt_be32(&b, 1u)) goto fail;
        if (!nbt_be16(&b, 16) || !nbt_buf_write(&b, "minecraft:plains", 16)) goto fail;
        if (!nbt_u8(&b, 0)) goto fail; /* end biomes */

        if (!nbt_u8(&b, 0)) goto fail; /* end section compound */
    }

    if (!nbt_tag_header(&b, 9, "block_entities") || !nbt_u8(&b, 10) || !nbt_be32(&b, 0u)) goto fail;
    if (!nbt_u8(&b, 0)) goto fail; /* end root */

    *out_nbt = b.data;
    *out_nbt_len = b.len;
    return 1;

fail:
    free(b.data);
    return 0;
}

static int32_t floor_div32_local(int32_t value, int32_t divisor) {
    int32_t q = value / divisor;
    int32_t r = value % divisor;
    if (r != 0 && ((r < 0) != (divisor < 0))) {
        q -= 1;
    }
    return q;
}

static int count_generated_region_indexed_chunks(const char *world_root,
                                                 int32_t region_x,
                                                 int32_t region_z,
                                                 int *out_indexed_count) {
    char region_path[1200];
    uint8_t locations[4096];
    FILE *fp;
    int indexed_count = 0;

    if (world_root == NULL || world_root[0] == '\0' || out_indexed_count == NULL) {
        return 0;
    }

#ifdef _WIN32
    snprintf(region_path, sizeof(region_path), "%s\\region\\r.%d.%d.mca", world_root, region_x, region_z);
#else
    snprintf(region_path, sizeof(region_path), "%s/region/r.%d.%d.mca", world_root, region_x, region_z);
#endif

    fp = fopen(region_path, "rb");
    if (fp == NULL) {
        return 0;
    }
    if (fread(locations, 1, sizeof(locations), fp) != sizeof(locations)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    for (int i = 0; i < 1024; ++i) {
        uint32_t location = ((uint32_t)locations[i * 4 + 0] << 24) |
                            ((uint32_t)locations[i * 4 + 1] << 16) |
                            ((uint32_t)locations[i * 4 + 2] << 8) |
                            (uint32_t)locations[i * 4 + 3];
        if (location != 0) {
            indexed_count += 1;
        }
    }

    *out_indexed_count = indexed_count;
    return 1;
}

static void log_generated_region_index_summary(const server_config_t *server_config,
                                               int32_t center_chunk_x,
                                               int32_t center_chunk_z,
                                               int radius) {
    char world_root[1024];
    int32_t min_chunk_x;
    int32_t max_chunk_x;
    int32_t min_chunk_z;
    int32_t max_chunk_z;
    int32_t min_region_x;
    int32_t max_region_x;
    int32_t min_region_z;
    int32_t max_region_z;
    int region_files = 0;

    if (server_config == NULL || radius < 0) {
        return;
    }
    if (!resolve_generated_world_root(server_config, world_root, sizeof(world_root))) {
        return;
    }

    min_chunk_x = center_chunk_x - radius;
    max_chunk_x = center_chunk_x + radius;
    min_chunk_z = center_chunk_z - radius;
    max_chunk_z = center_chunk_z + radius;

    min_region_x = floor_div32_local(min_chunk_x, 32);
    max_region_x = floor_div32_local(max_chunk_x, 32);
    min_region_z = floor_div32_local(min_chunk_z, 32);
    max_region_z = floor_div32_local(max_chunk_z, 32);

    for (int32_t rz = min_region_z; rz <= max_region_z; ++rz) {
        for (int32_t rx = min_region_x; rx <= max_region_x; ++rx) {
            int indexed_count = 0;
            if (count_generated_region_indexed_chunks(world_root, rx, rz, &indexed_count)) {
                printf("Generated region r.%d.%d.mca indexed chunks: %d/1024.\n",
                       rx,
                       rz,
                       indexed_count);
                region_files += 1;
            }
        }
    }

    if (region_files == 0) {
        printf("Generated region index summary: no region files found near (%d,%d) radius=%d.\n",
               center_chunk_x,
               center_chunk_z,
               radius);
    }
}

static int ensure_generated_world_region_chunk_stub(const char *world_root,
                                                    int32_t chunk_x,
                                                    int32_t chunk_z) {
    char region_path[1200];
    uint8_t *chunk_nbt = NULL;
    size_t chunk_nbt_len = 0;
    uint8_t *compressed_nbt = NULL;
    size_t compressed_nbt_len = 0;
    uint8_t header[8192];
    uint8_t pad[4096];
    size_t chunk_payload_len;
    size_t chunk_total_len;
    uint8_t sector_count;
    FILE *fp;
    uint32_t timestamp;
    int32_t region_x;
    int32_t region_z;
    int32_t local_x;
    int32_t local_z;
    int entry_index;
    uint32_t existing_loc;
    long file_size;
    uint32_t start_sector;

    if (world_root == NULL || world_root[0] == '\0') {
        return 0;
    }

    region_x = floor_div32_local(chunk_x, 32);
    region_z = floor_div32_local(chunk_z, 32);
    local_x = chunk_x - region_x * 32;
    local_z = chunk_z - region_z * 32;
    if (local_x < 0 || local_x > 31 || local_z < 0 || local_z > 31) {
        return 0;
    }
    entry_index = local_x + local_z * 32;

#ifdef _WIN32
    snprintf(region_path, sizeof(region_path), "%s\\region\\r.%d.%d.mca", world_root, region_x, region_z);
#else
    snprintf(region_path, sizeof(region_path), "%s/region/r.%d.%d.mca", world_root, region_x, region_z);
#endif

    fp = fopen(region_path, "rb");
    if (fp == NULL) {
        memset(header, 0, sizeof(header));
        fp = fopen(region_path, "wb");
        if (fp == NULL) {
            return 0;
        }
        (void)fwrite(header, 1, sizeof(header), fp);
        fclose(fp);
    } else {
        fclose(fp);
    }

    fp = fopen(region_path, "rb+");
    if (fp == NULL) {
        return 0;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return 0;
    }

    existing_loc = ((uint32_t)header[entry_index * 4 + 0] << 24) |
                   ((uint32_t)header[entry_index * 4 + 1] << 16) |
                   ((uint32_t)header[entry_index * 4 + 2] << 8) |
                   (uint32_t)header[entry_index * 4 + 3];
    if (existing_loc != 0) {
        fclose(fp);
        return 1;
    }

    if (!build_generated_chunk_nbt_payload(chunk_x, chunk_z, &chunk_nbt, &chunk_nbt_len)) {
        fclose(fp);
        return 0;
    }
    if (!zlib_compress_buffer(chunk_nbt, chunk_nbt_len, &compressed_nbt, &compressed_nbt_len)) {
        free(chunk_nbt);
        fclose(fp);
        return 0;
    }
    free(chunk_nbt);

    chunk_payload_len = 1u + compressed_nbt_len;
    chunk_total_len = 4u + chunk_payload_len;
    sector_count = (uint8_t)((chunk_total_len + 4095u) / 4096u);
    if (sector_count == 0 || sector_count > 255u) {
        free(compressed_nbt);
        fclose(fp);
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        free(compressed_nbt);
        fclose(fp);
        return 0;
    }
    file_size = ftell(fp);
    if (file_size < 8192) {
        file_size = 8192;
    }
    start_sector = (uint32_t)(((size_t)file_size + 4095u) / 4096u);
    if (start_sector > 0xFFFFFFu) {
        free(compressed_nbt);
        fclose(fp);
        return 0;
    }

    {
        size_t aligned_offset = (size_t)start_sector * 4096u;
        size_t cur = (size_t)file_size;
        memset(pad, 0, sizeof(pad));
        while (cur < aligned_offset) {
            size_t nwrite = aligned_offset - cur;
            if (nwrite > sizeof(pad)) {
                nwrite = sizeof(pad);
            }
            (void)fwrite(pad, 1, nwrite, fp);
            cur += nwrite;
        }
    }

    {
        uint8_t lenbuf[4];
        write_be32_ptr(lenbuf, (uint32_t)chunk_payload_len);
        (void)fwrite(lenbuf, 1, sizeof(lenbuf), fp);
    }
    {
        uint8_t ctype = 2;
        (void)fwrite(&ctype, 1, 1, fp);
    }
    (void)fwrite(compressed_nbt, 1, compressed_nbt_len, fp);

    {
        size_t pad_len = (size_t)sector_count * 4096u - chunk_total_len;
        memset(pad, 0, sizeof(pad));
        while (pad_len > 0) {
            size_t nwrite = pad_len > sizeof(pad) ? sizeof(pad) : pad_len;
            (void)fwrite(pad, 1, nwrite, fp);
            pad_len -= nwrite;
        }
    }

    header[entry_index * 4 + 0] = (uint8_t)((start_sector >> 16) & 0xFFu);
    header[entry_index * 4 + 1] = (uint8_t)((start_sector >> 8) & 0xFFu);
    header[entry_index * 4 + 2] = (uint8_t)(start_sector & 0xFFu);
    header[entry_index * 4 + 3] = sector_count;

    timestamp = (uint32_t)time(NULL);
    write_be32_ptr(header + 4096 + entry_index * 4, timestamp);

    if (fseek(fp, 0, SEEK_SET) == 0) {
        (void)fwrite(header, 1, sizeof(header), fp);
    }

    fclose(fp);
    free(compressed_nbt);
    return 1;
}

static void ensure_generated_world_level_dat(const char *world_root) {
    char level_dat_path[1200];
    uint8_t nbt[256];
    size_t n = 0;
    int32_t spawn_x = 8;
    int32_t spawn_z = 8;
    int32_t spawn_y = generated_world_surface_y(spawn_x, spawn_z) + 2;

    if (world_root == NULL || world_root[0] == '\0') {
        return;
    }

#ifdef _WIN32
    snprintf(level_dat_path, sizeof(level_dat_path), "%s\\level.dat", world_root);
#else
    snprintf(level_dat_path, sizeof(level_dat_path), "%s/level.dat", world_root);
#endif

    {
        FILE *fp = fopen(level_dat_path, "rb");
        if (fp != NULL) {
            fclose(fp);
            return;
        }
    }

    /* Root compound with empty name. */
    nbt[n++] = 10;
    write_be16_ptr(nbt + n, 0); n += 2;

    /* Data compound. */
    nbt[n++] = 10;
    write_be16_ptr(nbt + n, 4); n += 2;
    memcpy(nbt + n, "Data", 4); n += 4;

    /* SpawnX */
    nbt[n++] = 3;
    write_be16_ptr(nbt + n, 6); n += 2;
    memcpy(nbt + n, "SpawnX", 6); n += 6;
    write_be32_ptr(nbt + n, (uint32_t)spawn_x); n += 4;

    /* SpawnY */
    nbt[n++] = 3;
    write_be16_ptr(nbt + n, 6); n += 2;
    memcpy(nbt + n, "SpawnY", 6); n += 6;
    write_be32_ptr(nbt + n, (uint32_t)spawn_y); n += 4;

    /* SpawnZ */
    nbt[n++] = 3;
    write_be16_ptr(nbt + n, 6); n += 2;
    memcpy(nbt + n, "SpawnZ", 6); n += 6;
    write_be32_ptr(nbt + n, (uint32_t)spawn_z); n += 4;

    /* SpawnAngle float=0.0 */
    nbt[n++] = 5;
    write_be16_ptr(nbt + n, 10); n += 2;
    memcpy(nbt + n, "SpawnAngle", 10); n += 10;
    write_be32_ptr(nbt + n, 0); n += 4;

    /* End Data compound, end root compound. */
    nbt[n++] = 0;
    nbt[n++] = 0;

    if (!gzip_write_file(level_dat_path, nbt, n)) {
        printf("WARNING: failed to write generated level.dat at %s\n", level_dat_path);
    } else {
        printf("Created generated level.dat at %s\n", level_dat_path);
    }
}

static void ensure_generated_world_session_lock(const char *world_root) {
    char lock_path[1200];
    uint8_t lock_bytes[8];
    uint64_t now_ms;
    FILE *fp;

    if (world_root == NULL || world_root[0] == '\0') {
        return;
    }

#ifdef _WIN32
    snprintf(lock_path, sizeof(lock_path), "%s\\session.lock", world_root);
#else
    snprintf(lock_path, sizeof(lock_path), "%s/session.lock", world_root);
#endif

    now_ms = (uint64_t)time(NULL) * 1000ull;
    write_be64_ptr(lock_bytes, now_ms);

    fp = fopen(lock_path, "wb");
    if (fp == NULL) {
        printf("WARNING: failed to write generated session.lock at %s\n", lock_path);
        return;
    }

    (void)fwrite(lock_bytes, 1, sizeof(lock_bytes), fp);
    fclose(fp);
}

static int resolve_generated_world_root(const server_config_t *server_config,
                                        char *world_root,
                                        size_t world_root_size) {
    char exe_dir[1024];

    if (server_config == NULL || world_root == NULL || world_root_size == 0) {
        return 0;
    }

    if (server_config->world_path[0] != '\0') {
        snprintf(world_root, world_root_size, "%s", server_config->world_path);
        return 1;
    }

    if (!get_executable_dir_main(exe_dir, sizeof(exe_dir))) {
        return 0;
    }
#ifdef _WIN32
    snprintf(world_root, world_root_size, "%s\\world", exe_dir);
#else
    snprintf(world_root, world_root_size, "%s/world", exe_dir);
#endif
    return 1;
}

static int load_cached_generated_chunk_packet(const server_config_t *server_config,
                                              int32_t chunk_x,
                                              int32_t chunk_z,
                                              uint8_t **out_packet,
                                              size_t *out_packet_len) {
    char world_root[1024];
    char cache_path[1200];
    FILE *fp;
    uint8_t header[20];
    uint32_t payload_len;
    int32_t stored_x;
    int32_t stored_z;
    uint8_t *payload;

    if (out_packet == NULL || out_packet_len == NULL) {
        return 0;
    }
    *out_packet = NULL;
    *out_packet_len = 0;

    if (!resolve_generated_world_root(server_config, world_root, sizeof(world_root))) {
        return 0;
    }

#ifdef _WIN32
    snprintf(cache_path, sizeof(cache_path), "%s\\generated_packets\\c.%d.%d.bin", world_root, chunk_x, chunk_z);
#else
    snprintf(cache_path, sizeof(cache_path), "%s/generated_packets/c.%d.%d.bin", world_root, chunk_x, chunk_z);
#endif

    fp = fopen(cache_path, "rb");
    if (fp == NULL) {
        return 0;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return 0;
    }

    if (!(header[0] == 'V' && header[1] == 'G' && header[2] == 'C' && header[3] == 'P')) {
        fclose(fp);
        return 0;
    }
    if (!(header[4] == 0 &&
          header[5] == 0 &&
          header[6] == 0 &&
          header[7] == (uint8_t)GENERATED_PACKET_CACHE_VERSION)) {
        fclose(fp);
        return 0;
    }

    stored_x = (int32_t)(((uint32_t)header[8] << 24) |
                         ((uint32_t)header[9] << 16) |
                         ((uint32_t)header[10] << 8) |
                         (uint32_t)header[11]);
    stored_z = (int32_t)(((uint32_t)header[12] << 24) |
                         ((uint32_t)header[13] << 16) |
                         ((uint32_t)header[14] << 8) |
                         (uint32_t)header[15]);
    payload_len = ((uint32_t)header[16] << 24) |
                  ((uint32_t)header[17] << 16) |
                  ((uint32_t)header[18] << 8) |
                  (uint32_t)header[19];

    if (stored_x != chunk_x || stored_z != chunk_z || payload_len == 0 || payload_len > (8u * 1024u * 1024u)) {
        fclose(fp);
        return 0;
    }

    payload = (uint8_t *)malloc((size_t)payload_len);
    if (payload == NULL) {
        fclose(fp);
        return 0;
    }

    if (fread(payload, 1, (size_t)payload_len, fp) != (size_t)payload_len) {
        free(payload);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    *out_packet = payload;
    *out_packet_len = (size_t)payload_len;
    return 1;
}

static void save_cached_generated_chunk_packet(const server_config_t *server_config,
                                               int32_t chunk_x,
                                               int32_t chunk_z,
                                               const uint8_t *packet,
                                               size_t packet_len) {
    char world_root[1024];
    char cache_dir[1100];
    char cache_path[1200];
    FILE *fp;
    uint8_t header[20];

    if (packet == NULL || packet_len == 0 || packet_len > (8u * 1024u * 1024u)) {
        return;
    }
    if (!resolve_generated_world_root(server_config, world_root, sizeof(world_root))) {
        return;
    }

#ifdef _WIN32
    snprintf(cache_dir, sizeof(cache_dir), "%s\\generated_packets", world_root);
    snprintf(cache_path, sizeof(cache_path), "%s\\c.%d.%d.bin", cache_dir, chunk_x, chunk_z);
#else
    snprintf(cache_dir, sizeof(cache_dir), "%s/generated_packets", world_root);
    snprintf(cache_path, sizeof(cache_path), "%s/c.%d.%d.bin", cache_dir, chunk_x, chunk_z);
#endif

    if (!create_dir_if_missing(cache_dir)) {
        return;
    }

    header[0] = 'V'; header[1] = 'G'; header[2] = 'C'; header[3] = 'P';
    header[4] = 0;
    header[5] = 0;
    header[6] = 0;
    header[7] = (uint8_t)GENERATED_PACKET_CACHE_VERSION;
    header[8] = (uint8_t)(((uint32_t)chunk_x >> 24) & 0xFF);
    header[9] = (uint8_t)(((uint32_t)chunk_x >> 16) & 0xFF);
    header[10] = (uint8_t)(((uint32_t)chunk_x >> 8) & 0xFF);
    header[11] = (uint8_t)((uint32_t)chunk_x & 0xFF);
    header[12] = (uint8_t)(((uint32_t)chunk_z >> 24) & 0xFF);
    header[13] = (uint8_t)(((uint32_t)chunk_z >> 16) & 0xFF);
    header[14] = (uint8_t)(((uint32_t)chunk_z >> 8) & 0xFF);
    header[15] = (uint8_t)((uint32_t)chunk_z & 0xFF);
    header[16] = (uint8_t)(((uint32_t)packet_len >> 24) & 0xFF);
    header[17] = (uint8_t)(((uint32_t)packet_len >> 16) & 0xFF);
    header[18] = (uint8_t)(((uint32_t)packet_len >> 8) & 0xFF);
    header[19] = (uint8_t)((uint32_t)packet_len & 0xFF);

    fp = fopen(cache_path, "wb");
    if (fp == NULL) {
        return;
    }

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header) ||
        fwrite(packet, 1, packet_len, fp) != packet_len) {
        fclose(fp);
        return;
    }

    fclose(fp);

    (void)ensure_generated_world_region_chunk_stub(world_root, chunk_x, chunk_z);
}

static void prewarm_generated_chunk_cache(const server_config_t *server_config,
                                          int32_t center_chunk_x,
                                          int32_t center_chunk_z,
                                          int radius) {
    int generated_count = 0;

    if (server_config == NULL || radius < 0) {
        return;
    }

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int32_t chunk_x = center_chunk_x + dx;
            int32_t chunk_z = center_chunk_z + dz;
            uint8_t *cached_packet = NULL;
            size_t cached_packet_len = 0;

            if (load_cached_generated_chunk_packet(server_config,
                                                   chunk_x,
                                                   chunk_z,
                                                   &cached_packet,
                                                   &cached_packet_len)) {
                char world_root[1024];
                if (resolve_generated_world_root(server_config, world_root, sizeof(world_root))) {
                    (void)ensure_generated_world_region_chunk_stub(world_root, chunk_x, chunk_z);
                }
                free(cached_packet);
                continue;
            }

            {
                size_t packet_len = 0;
                fprintf(stderr, "[PLAY] Generating chunk (%d,%d)...\n", chunk_x, chunk_z);
                fflush(stderr);
                uint8_t *packet = build_generated_overworld_chunk_packet(chunk_x, chunk_z, &packet_len);
                if (packet == NULL) {
                    fprintf(stderr, "[PLAY] build_generated_overworld_chunk_packet returned NULL for (%d,%d), trying fallback...\n", chunk_x, chunk_z);
                    fflush(stderr);
                    uint8_t *chunk_nbt = NULL;
                    size_t chunk_nbt_len = 0;
                    if (build_generated_chunk_nbt_payload(chunk_x, chunk_z, &chunk_nbt, &chunk_nbt_len)) {
                        fprintf(stderr, "[PLAY] Using fallback build_chunk_data_packet for (%d,%d)\n", chunk_x, chunk_z);
                        fflush(stderr);
                        packet = build_chunk_data_packet(chunk_nbt,
                                                         chunk_nbt_len,
                                                         chunk_x,
                                                         chunk_z,
                                                         &packet_len);
                        free(chunk_nbt);
                    } else {
                        fprintf(stderr, "[PLAY] build_generated_chunk_nbt_payload failed for (%d,%d)\n", chunk_x, chunk_z);
                        fflush(stderr);
                    }
                } else {
                    fprintf(stderr, "[PLAY] Generated chunk (%d,%d) successfully, size=%zu\n", chunk_x, chunk_z, packet_len);
                    fflush(stderr);
                }
                if (packet != NULL) {
                    save_cached_generated_chunk_packet(server_config,
                                                       chunk_x,
                                                       chunk_z,
                                                       packet,
                                                       packet_len);
                    free(packet);
                    generated_count += 1;
                }
            }
        }
    }

    if (generated_count > 0) {
        printf("Prewarmed %d generated chunk cache files around (%d,%d) radius=%d.\n",
               generated_count,
               center_chunk_x,
               center_chunk_z,
               radius);
    }
}

static void ensure_generated_world_scaffold(const server_config_t *server_config) {
    char exe_dir[1024];
    char world_root[1024];
    char region_dir[1024];
    char generated_packets_dir[1024];
    char dim_nether[1024];
    char dim_nether_region[1024];
    char dim_end[1024];
    char dim_end_region[1024];
    char marker_path[1024];
    FILE *fp;
    int force_debug;
    int resolved_mode;

    if (server_config == NULL) {
        return;
    }

    force_debug = server_config->force_debug_spawn || env_flag_enabled("VECTORA_FORCE_DEBUG_SPAWN");
    resolved_mode = force_debug ? WORLD_SOURCE_MODE_DEBUG : server_config->world_source_mode;
    if (resolved_mode != WORLD_SOURCE_MODE_GENERATED) {
        return;
    }

    (void)exe_dir;
    if (!resolve_generated_world_root(server_config, world_root, sizeof(world_root))) {
        return;
    }

    if (!create_dir_if_missing(world_root)) {
        printf("WARNING: failed to create generated world root at %s\n", world_root);
        return;
    }

#ifdef _WIN32
    snprintf(region_dir, sizeof(region_dir), "%s\\region", world_root);
    snprintf(generated_packets_dir, sizeof(generated_packets_dir), "%s\\generated_packets", world_root);
    snprintf(dim_nether, sizeof(dim_nether), "%s\\DIM-1", world_root);
    snprintf(dim_nether_region, sizeof(dim_nether_region), "%s\\region", dim_nether);
    snprintf(dim_end, sizeof(dim_end), "%s\\DIM1", world_root);
    snprintf(dim_end_region, sizeof(dim_end_region), "%s\\region", dim_end);
    snprintf(marker_path, sizeof(marker_path), "%s\\GENERATED_WORLD.txt", world_root);
#else
    snprintf(region_dir, sizeof(region_dir), "%s/region", world_root);
    snprintf(generated_packets_dir, sizeof(generated_packets_dir), "%s/generated_packets", world_root);
    snprintf(dim_nether, sizeof(dim_nether), "%s/DIM-1", world_root);
    snprintf(dim_nether_region, sizeof(dim_nether_region), "%s/region", dim_nether);
    snprintf(dim_end, sizeof(dim_end), "%s/DIM1", world_root);
    snprintf(dim_end_region, sizeof(dim_end_region), "%s/region", dim_end);
    snprintf(marker_path, sizeof(marker_path), "%s/GENERATED_WORLD.txt", world_root);
#endif

    (void)create_dir_if_missing(region_dir);
    (void)create_dir_if_missing(generated_packets_dir);
    (void)create_dir_if_missing(dim_nether);
    (void)create_dir_if_missing(dim_nether_region);
    (void)create_dir_if_missing(dim_end);
    (void)create_dir_if_missing(dim_end_region);

    fp = fopen(marker_path, "rb");
    if (fp != NULL) {
        fclose(fp);
    } else {
        fp = fopen(marker_path, "wb");
        if (fp != NULL) {
            const char *text =
                "Vectora generated world scaffold\n"
                "This folder was created automatically for world_source=generated.\n"
                "Chunk terrain is generated procedurally and persisted to generated packet cache and region files.\n";
            fwrite(text, 1, strlen(text), fp);
            fclose(fp);
        }
    }

    ensure_generated_world_level_dat(world_root);
    ensure_generated_world_session_lock(world_root);
    if (ensure_generated_world_region_chunk_stub(world_root, 0, 0)) {
        printf("Generated region stub entry ensured for chunk (0,0).\n");
    }

    printf("Generated world scaffold ready at %s\n", world_root);
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
    int32_t packet_len = -1;

    if (buf == NULL || out_packet_id == NULL || len == 0) {
        return 0;
    }

    // Outer packet length must be valid and fully present in this buffer.
    packet_len = read_varint(&p, &len);
    if (packet_len < 0) {
        return 0;
    }
    if ((size_t)packet_len > len) {
        return 0;
    }
    len = (size_t)packet_len;
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
        if (*out_packet_id < 0) {
            return 0;
        }
        return 1;
    }

    uint8_t inflated[8192];
    uLongf inflated_len = sizeof(inflated);
    if ((size_t)data_len > sizeof(inflated)) {
        return 0;
    }
    int zres = uncompress(inflated, &inflated_len, p, (uLong)len);
    if (zres != Z_OK) {
        printf("[read_post_compression_packet_id] zlib uncompress error: %d\n", zres);
        return 0;
    }

    if ((int32_t)inflated_len != data_len) {
        return 0;
    }

    const uint8_t *ip = inflated;
    size_t ilen = (size_t)inflated_len;
    if (ilen == 0) {
        return 0;
    }

    *out_packet_id = read_varint(&ip, &ilen);
    if (*out_packet_id < 0) {
        return 0;
    }
    return 1;
}

static size_t double_frame_packet(uint8_t *dst, size_t dst_cap, const uint8_t *src, size_t len) {
    size_t off = 0;
    uint8_t inner[4096];
    size_t inner_off = 0;
    int compress = (int)len >= compression_threshold;

    if (dst == NULL || src == NULL || dst_cap == 0) {
        return 0;
    }

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
        if (inner_off > sizeof(inner)) {
            return 0;
        }
        // Write total length
        off += write_varint(dst + off, (int)inner_off);
        if (off + inner_off > dst_cap) {
            printf("[double_frame_packet] dst too small: need=%zu, cap=%zu\n", off + inner_off, dst_cap);
            return 0;
        }
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
        if (inner_off > sizeof(inner)) {
            return 0;
        }
        // Write total length (length of inner frame)
        off += write_varint(dst + off, (int)inner_off);
        if (off + inner_off > dst_cap) {
            printf("[double_frame_packet] dst too small: need=%zu, cap=%zu\n", off + inner_off, dst_cap);
            return 0;
        }
        memcpy(dst + off, inner, inner_off);
    }
    if (g_log_packet_framing) {
        printf("[double_frame_packet] inner (hex): ");
        for (size_t i = 0; i < inner_off; ++i) printf("%02X ", inner[i]);
        printf("\n[double_frame_packet] outer (hex): ");
        for (size_t i = 0; i < off + inner_off && i < dst_cap; ++i) printf("%02X ", dst[i]);
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

static size_t write_mc_string_raw(uint8_t *dst, const char *text) {
    size_t text_len = strlen(text);
    size_t offset = 0;
    offset += write_varint(dst + offset, (int32_t)text_len);
    memcpy(dst + offset, text, text_len);
    offset += text_len;
    return offset;
}

static void send_login_disconnect(socket_handle_t socket_fd, const char *reason_json) {
    uint8_t packet[512];
    uint8_t framed[520];
    size_t packet_len = 0;
    size_t framed_len = 0;

    packet_len += write_varint(packet + packet_len, 0x00);
    packet_len += write_mc_string_raw(packet + packet_len, reason_json);

    framed_len += write_varint(framed + framed_len, (int32_t)packet_len);
    memcpy(framed + framed_len, packet, packet_len);
    framed_len += packet_len;

    (void)send_all(socket_fd, framed, framed_len);
}

static void send_post_compression_packet(socket_handle_t socket_fd, const uint8_t *packet, size_t packet_len);

static void build_text_component_json(const char *text, char *out_json, size_t out_json_size) {
    size_t out = 0;
    if (out_json_size < 3) {
        return;
    }

    out_json[out++] = '{';
    out_json[out++] = '"';
    out_json[out++] = 't';
    out_json[out++] = 'e';
    out_json[out++] = 'x';
    out_json[out++] = 't';
    out_json[out++] = '"';
    out_json[out++] = ':';
    out_json[out++] = '"';

    for (size_t i = 0; text[i] != '\0' && out + 2 < out_json_size; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '"' || ch == '\\') {
            if (out + 2 >= out_json_size) {
                break;
            }
            out_json[out++] = '\\';
            out_json[out++] = (char)ch;
        } else if (ch < 0x20) {
            out_json[out++] = ' ';
        } else {
            out_json[out++] = (char)ch;
        }
    }

    if (out + 2 >= out_json_size) {
        out = out_json_size - 3;
    }
    out_json[out++] = '"';
    out_json[out++] = '}';
    out_json[out] = '\0';
}

static size_t write_network_nbt_string(uint8_t *outbuf, size_t outbuf_size, const char *text) {
    size_t text_len;

    if (outbuf == NULL || text == NULL || outbuf_size < 3) {
        return 0;
    }

    text_len = strlen(text);
    if (text_len > 65535 || outbuf_size < (size_t)(3 + text_len)) {
        return 0;
    }

    outbuf[0] = 0x08; // TAG_String (network NBT root)
    outbuf[1] = (uint8_t)((text_len >> 8) & 0xFF);
    outbuf[2] = (uint8_t)(text_len & 0xFF);
    memcpy(outbuf + 3, text, text_len);
    return 3 + text_len;
}

static void send_play_disconnect(socket_handle_t socket_fd,
                                 int packet_id,
                                 const char *reason_text,
                                 int use_network_nbt_string) {
    uint8_t packet[512];
    size_t packet_len = 0;

    packet_len += write_varint(packet + packet_len, packet_id);
    if (use_network_nbt_string) {
        size_t nbt_len = write_network_nbt_string(packet + packet_len,
                                                  sizeof(packet) - packet_len,
                                                  reason_text);
        if (nbt_len == 0) {
            return;
        }
        packet_len += nbt_len;
    } else {
        char reason_json[320];
        build_text_component_json(reason_text, reason_json, sizeof(reason_json));
        packet_len += write_mc_string_raw(packet + packet_len, reason_json);
    }
    send_post_compression_packet(socket_fd, packet, packet_len);
}

static void send_system_chat_message(socket_handle_t socket_fd,
                                     const char *text,
                                     int overlay) {
    uint8_t packet[768];
    size_t packet_len = 0;
    size_t nbt_len;

    if (text == NULL) {
        return;
    }

    packet_len += write_varint(packet + packet_len, PLAY774_S2C_SYSTEM_CHAT);
    nbt_len = write_network_nbt_string(packet + packet_len,
                                       sizeof(packet) - packet_len,
                                       text);
    if (nbt_len == 0) {
        return;
    }
    packet_len += nbt_len;
    packet[packet_len++] = overlay ? 0x01 : 0x00;

    send_post_compression_packet(socket_fd, packet, packet_len);
}

static void broadcast_system_chat_message(const char *text, int overlay) {
    socket_handle_t sockets[MAX_PLAY_CHAT_SOCKETS];
    size_t count = 0;
    size_t i;

    if (text == NULL) {
        return;
    }

    PLAY_CHAT_LOCK();
    count = g_play_chat_socket_count;
    if (count > MAX_PLAY_CHAT_SOCKETS) {
        count = MAX_PLAY_CHAT_SOCKETS;
    }
    memcpy(sockets, g_play_chat_sockets, count * sizeof(sockets[0]));
    PLAY_CHAT_UNLOCK();

    for (i = 0; i < count; ++i) {
        send_system_chat_message(sockets[i], text, overlay);
    }
}

static void broadcast_moderator_system_message(const char *text, int overlay) {
    socket_handle_t sockets[MAX_PLAY_CHAT_SOCKETS];
    char usernames[MAX_PLAY_CHAT_SOCKETS][32];
    char moderators[MAX_MODERATORS][32];
    size_t socket_count = 0;
    size_t moderator_count = 0;
    size_t i;

    if (text == NULL) {
        return;
    }

    PLAY_CHAT_LOCK();
    socket_count = g_play_chat_socket_count;
    if (socket_count > MAX_PLAY_CHAT_SOCKETS) {
        socket_count = MAX_PLAY_CHAT_SOCKETS;
    }
    memcpy(sockets, g_play_chat_sockets, socket_count * sizeof(sockets[0]));
    memcpy(usernames,
           g_play_chat_socket_usernames,
           socket_count * sizeof(usernames[0]));

    moderator_count = g_moderator_username_count;
    if (moderator_count > MAX_MODERATORS) {
        moderator_count = MAX_MODERATORS;
    }
    memcpy(moderators,
           g_moderator_usernames,
           moderator_count * sizeof(moderators[0]));
    PLAY_CHAT_UNLOCK();

    for (i = 0; i < socket_count; ++i) {
        size_t j;
        int is_mod = 0;
        for (j = 0; j < moderator_count; ++j) {
            if (username_equals_ci(usernames[i], moderators[j])) {
                is_mod = 1;
                break;
            }
        }
        if (is_mod) {
            send_system_chat_message(sockets[i], text, overlay);
        }
    }
}

static int entity_is_within_stream_radius(const client_session_t *session,
                                          double x,
                                          double z) {
    int32_t chunk_x;
    int32_t chunk_z;

    if (session == NULL) {
        return 0;
    }

    chunk_x = (int32_t)floor(x / 16.0);
    chunk_z = (int32_t)floor(z / 16.0);

    return (abs(chunk_x - session->stream_state.stream_center_chunk_x) <= session->stream_state.chunk_stream_radius) &&
           (abs(chunk_z - session->stream_state.stream_center_chunk_z) <= session->stream_state.chunk_stream_radius);
}

static void reconcile_entity_visibility(client_session_t *session,
                                        socket_handle_t socket_fd,
                                        int32_t entity_id,
                                        int32_t entity_type,
                                        int glow,
                                        int *visible_flag) {
    double ex = 0.0;
    double ey = 0.0;
    double ez = 0.0;
    int in_range = 0;

    if (session == NULL || visible_flag == NULL || entity_id == 0) {
        return;
    }

    if (!find_entity_position(&session->entity_registry, entity_id, &ex, &ey, &ez)) {
        if (*visible_flag) {
            uint8_t rem_buf[64];
            size_t rem_len = build_entity_destroy_packet(rem_buf,
                                                         sizeof(rem_buf),
                                                         &entity_id,
                                                         1);
            send_post_compression_packet(socket_fd, rem_buf, rem_len);
            *visible_flag = 0;
        }
        return;
    }

    in_range = entity_is_within_stream_radius(session, ex, ez);
    if (in_range && !*visible_flag) {
        uint8_t se_buf[128];
        size_t se_len = build_spawn_entity_packet(se_buf,
                                                  sizeof(se_buf),
                                                  entity_id,
                                                  entity_type,
                                                  ex,
                                                  ey,
                                                  ez,
                                                  0,
                                                  0,
                                                  0,
                                                  0);
        send_post_compression_packet(socket_fd, se_buf, se_len);
        session->entity_stats_spawn_packets_sent += 1;
        if (entity_type == PLAY774_ENTITY_TYPE_ITEM) {
            uint8_t item_md_buf[64];
            size_t item_md_len = build_set_item_entity_slot_packet(item_md_buf,
                                                                   sizeof(item_md_buf),
                                                                   entity_id,
                                                                   PLAY774_ITEM_ID_STONE,
                                                                   1);
            if (item_md_len > 0) {
                send_post_compression_packet(socket_fd, item_md_buf, item_md_len);
            }
        }
        if (glow) {
            uint8_t md_buf[32];
            size_t md_len = build_set_entity_glowing_packet(md_buf,
                                                            sizeof(md_buf),
                                                            entity_id,
                                                            1);
            send_post_compression_packet(socket_fd, md_buf, md_len);
        }
        *visible_flag = 1;
    } else if (!in_range && *visible_flag) {
        uint8_t rem_buf[64];
        size_t rem_len = build_entity_destroy_packet(rem_buf,
                                                     sizeof(rem_buf),
                                                     &entity_id,
                                                     1);
        send_post_compression_packet(socket_fd, rem_buf, rem_len);
        session->entity_stats_destroy_packets_sent += 1;
        *visible_flag = 0;
    }
}

static void reconcile_tracked_spawned_entities_visibility(client_session_t *session,
                                                          socket_handle_t socket_fd) {
    size_t i;

    if (session == NULL) {
        return;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        reconcile_entity_visibility(session,
                                    socket_fd,
                                    session->spawned_entity_ids[i],
                                    session->spawned_entity_types[i],
                                    0,
                                    &session->spawned_entity_visible[i]);
    }
}

static void tick_tracked_mob_movement(client_session_t *session,
                                      socket_handle_t socket_fd,
                                      uint64_t tick,
                                      int has_player_anchor,
                                      double player_x,
                                      double player_z) {
    size_t i;

    if (session == NULL) {
        return;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        int32_t ent_id = session->spawned_entity_ids[i];
        int32_t ent_type = session->spawned_entity_types[i];
        double ex = 0.0;
        double ey = 0.0;
        double ez = 0.0;
        double base_x;
        double base_z;
        double phase;
        double radius;
        double target_x;
        double target_z;
        double nx;
        double ny;
        double nz;
        double ground_y;
        double step_up;
        double vertical_velocity;
        int was_visible;
        int on_ground;

        if (ent_type == PLAY774_ENTITY_TYPE_ITEM) {
            continue;
        }

        if (!find_entity_position(&session->entity_registry, ent_id, &ex, &ey, &ez)) {
            continue;
        }

        base_x = has_player_anchor ? player_x : (double)(session->stream_state.stream_center_chunk_x * 16 + 8);
        base_z = has_player_anchor ? player_z : (double)(session->stream_state.stream_center_chunk_z * 16 + 8);
        phase = ((double)((tick + ((uint64_t)(ent_id & 0x7FFF) * 13ULL)) % 2400ULL)) * 0.00261799387799;
        {
            double base_radius = (double)session->server_config.entity_roam_radius_blocks;
            if (base_radius < 1.0) {
                base_radius = 1.0;
            }
            radius = base_radius * (0.55 + (double)(i % 6) * 0.10);
        }
        target_x = base_x + cos(phase + (double)i * 0.41) * radius;
        target_z = base_z + sin((phase * 0.93) + (double)i * 0.29) * radius;

        // Smoothly steer each mob toward its current orbit target.
        nx = ex + (target_x - ex) * 0.10;
        nz = ez + (target_z - ez) * 0.10;
        ny = ey;
        ground_y = resolve_generated_ground_y(nx, nz);
        step_up = ground_y - ey;
        vertical_velocity = session->spawned_entity_vertical_velocity[i];
        on_ground = session->spawned_entity_on_ground[i];

        if (step_up > 0.0 && step_up <= 1.25) {
            ny = ground_y;
            vertical_velocity = 0.0;
            on_ground = 1;
        } else if (ey > ground_y + 0.001) {
            vertical_velocity -= 0.08;
            if (vertical_velocity < -1.5) {
                vertical_velocity = -1.5;
            }
            ny = ey + vertical_velocity;
            if (ny <= ground_y) {
                ny = ground_y;
                vertical_velocity = 0.0;
                on_ground = 1;
            } else {
                on_ground = 0;
            }
        } else {
            ny = ground_y;
            vertical_velocity = 0.0;
            on_ground = 1;
        }

        session->spawned_entity_vertical_velocity[i] = vertical_velocity;
        session->spawned_entity_on_ground[i] = on_ground;

        (void)entity_registry_update_xyz(&session->entity_registry, ent_id, nx, ny, nz);

        was_visible = session->spawned_entity_visible[i];
        reconcile_entity_visibility(session,
                                    socket_fd,
                                    ent_id,
                                    ent_type,
                                    0,
                                    &session->spawned_entity_visible[i]);

        if (was_visible && session->spawned_entity_visible[i]) {
            double dx_raw = (nx - ex) * 4096.0;
            double dy_raw = (ny - ey) * 4096.0;
            double dz_raw = (nz - ez) * 4096.0;
            uint8_t mv_buf[64];
            size_t mv_len;

            if (dx_raw >= -32767.0 && dx_raw <= 32767.0 &&
                dy_raw >= -32767.0 && dy_raw <= 32767.0 &&
                dz_raw >= -32767.0 && dz_raw <= 32767.0) {
                mv_len = build_move_entity_pos_packet(mv_buf,
                                                      sizeof(mv_buf),
                                                      ent_id,
                                                      (int16_t)dx_raw,
                                                      (int16_t)dy_raw,
                                                      (int16_t)dz_raw,
                                                      on_ground);
            } else {
                mv_len = build_entity_position_sync_packet(mv_buf,
                                                           sizeof(mv_buf),
                                                           ent_id,
                                                           nx,
                                                           ny,
                                                           nz,
                                                           0.0,
                                                           vertical_velocity,
                                                           0.0,
                                                           0.0f,
                                                           0.0f,
                                                           on_ground);
            }

            send_post_compression_packet(socket_fd, mv_buf, mv_len);
            session->entity_stats_move_packets_sent += 1;

            {
                double look_x = has_player_anchor ? player_x : nx + (target_x - nx);
                double look_z = has_player_anchor ? player_z : nz + (target_z - nz);
                double lx = look_x - nx;
                double lz = look_z - nz;
                double yaw = -atan2(lx, lz) * 180.0 / 3.14159265359;
                uint8_t hr_buf[16];
                size_t hr_len;

                if (yaw < 0.0) {
                    yaw += 360.0;
                }

                hr_len = build_set_head_rotation_packet(hr_buf, sizeof(hr_buf), ent_id, (float)yaw);
                send_post_compression_packet(socket_fd, hr_buf, hr_len);
                session->entity_stats_head_packets_sent += 1;
            }
        } else if (!session->spawned_entity_visible[i]) {
            session->entity_stats_update_culled += 1;
        }
    }
}

static void enforce_tracked_entity_limits(client_session_t *session,
                                          socket_handle_t socket_fd,
                                          time_t now_ts,
                                          int has_player_anchor,
                                          double player_x,
                                          double player_z) {
    int despawn_seconds;
    int despawn_distance_blocks;
    int max_tracked;
    double despawn_distance_sq = 0.0;
    size_t i = 0;

    if (session == NULL) {
        return;
    }

    despawn_seconds = session->server_config.entity_despawn_seconds;
    despawn_distance_blocks = session->server_config.entity_despawn_distance_blocks;
    max_tracked = session->server_config.entity_max_tracked;

    if (max_tracked < 1) {
        max_tracked = 1;
    }
    if (max_tracked > (int)(sizeof(session->spawned_entity_ids) / sizeof(session->spawned_entity_ids[0]))) {
        max_tracked = (int)(sizeof(session->spawned_entity_ids) / sizeof(session->spawned_entity_ids[0]));
    }

    if (despawn_distance_blocks > 0) {
        despawn_distance_sq = (double)despawn_distance_blocks * (double)despawn_distance_blocks;
    }

    while (i < session->spawned_entity_count) {
        int remove_entity = 0;
        int32_t ent_id = session->spawned_entity_ids[i];
        int32_t ent_type = session->spawned_entity_types[i];
        int was_visible = session->spawned_entity_visible[i];
        double ex = 0.0, ey = 0.0, ez = 0.0;

        if (ent_type == PLAY774_ENTITY_TYPE_ITEM) {
            i += 1;
            continue;
        }

        if (despawn_seconds > 0) {
            time_t born = session->spawned_entity_birth_at[i];
            if (born > 0 && now_ts >= born && (now_ts - born) >= despawn_seconds) {
                remove_entity = 1;
            }
        }

        if (!remove_entity && despawn_distance_sq > 0.0 && has_player_anchor) {
            if (find_entity_position(&session->entity_registry, ent_id, &ex, &ey, &ez)) {
                double dx = ex - player_x;
                double dz = ez - player_z;
                double dist_sq = dx * dx + dz * dz;
                if (dist_sq > despawn_distance_sq) {
                    remove_entity = 1;
                }
            }
        }

        if (!remove_entity && session->spawned_entity_count > (size_t)max_tracked) {
            remove_entity = 1;
        }

        if (remove_entity) {
            if (was_visible) {
                uint8_t rem_buf[64];
                size_t rem_len = build_entity_destroy_packet(rem_buf, sizeof(rem_buf), &ent_id, 1);
                send_post_compression_packet(socket_fd, rem_buf, rem_len);
                session->entity_stats_destroy_packets_sent += 1;
            }
            (void)entity_manager_queue_remove(&session->entity_registry,
                                              &session->entity_manager,
                                              ent_id);
            (void)untrack_spawned_entity_id(session, ent_id);
            continue;
        }

        i += 1;
    }
}

static size_t count_tracked_roaming_mobs(const client_session_t *session) {
    size_t i;
    size_t count = 0;

    if (session == NULL) {
        return 0;
    }

    for (i = 0; i < session->spawned_entity_count; ++i) {
        if (session->spawned_entity_types[i] != PLAY774_ENTITY_TYPE_ITEM) {
            count += 1;
        }
    }

    return count;
}

static size_t refill_tracked_roaming_mobs(client_session_t *session,
                                          socket_handle_t socket_fd,
                                          uint64_t tick,
                                          int has_player_anchor,
                                          double player_x,
                                          double player_z) {
    size_t active_mobs;
    size_t to_spawn;
    size_t i;
    size_t tracked_cap;
    size_t max_tracked;
    size_t spawned = 0;
    double base_x;
    double base_z;
    double base_radius;

    if (session == NULL) {
        return 0;
    }

    if (session->server_config.entity_target_active_mobs <= 0) {
        return 0;
    }

    tracked_cap = sizeof(session->spawned_entity_ids) / sizeof(session->spawned_entity_ids[0]);
    max_tracked = (size_t)session->server_config.entity_max_tracked;
    if (max_tracked > tracked_cap) {
        max_tracked = tracked_cap;
    }

    active_mobs = count_tracked_roaming_mobs(session);
    if (active_mobs >= (size_t)session->server_config.entity_target_active_mobs) {
        return 0;
    }

    to_spawn = (size_t)session->server_config.entity_target_active_mobs - active_mobs;
    if (to_spawn > 8) {
        to_spawn = 8;
    }

    if (session->spawned_entity_count >= max_tracked) {
        return 0;
    }

    if (to_spawn > (max_tracked - session->spawned_entity_count)) {
        to_spawn = max_tracked - session->spawned_entity_count;
    }

    base_x = has_player_anchor ? player_x : (double)(session->stream_state.stream_center_chunk_x * 16 + 8);
    base_z = has_player_anchor ? player_z : (double)(session->stream_state.stream_center_chunk_z * 16 + 8);
    base_radius = (double)session->server_config.entity_roam_radius_blocks;
    if (base_radius < 2.0) {
        base_radius = 2.0;
    }

    for (i = 0; i < to_spawn; ++i) {
        int32_t mob_id = 0;
        int32_t mob_type = pick_roaming_mob_type((size_t)(tick + i));
        int tracked_idx = -1;
        double phase = ((double)((tick + i * 97ULL) % 2400ULL)) * 0.00261799387799;
        double radius = base_radius * (0.70 + (double)(i % 4) * 0.10);
        double sx = base_x + cos(phase) * radius;
        double sz = base_z + sin(phase) * radius;
        double sy = resolve_generated_ground_y(sx, sz);

        if (!entity_manager_queue_spawn_auto(&session->entity_registry,
                                             &session->entity_manager,
                                             ENTITY_KIND_MOB,
                                             sx,
                                             sy,
                                             sz,
                                             &mob_id)) {
            continue;
        }

        if (track_spawned_entity_id(session, mob_id, mob_type)) {
            tracked_idx = find_tracked_spawned_entity_index(session, mob_id);
        }

        if (tracked_idx >= 0) {
            session->spawned_entity_visible[tracked_idx] = 0;
            reconcile_entity_visibility(session,
                                        socket_fd,
                                        mob_id,
                                        mob_type,
                                        0,
                                        &session->spawned_entity_visible[tracked_idx]);
            spawned += 1;
        }
    }

    return spawned;
}

static void send_post_compression_packet(socket_handle_t socket_fd, const uint8_t *packet, size_t packet_len) {
    if (packet == NULL || packet_len == 0) {
        if (g_log_packet_framing) {
            fprintf(stderr, "[WARN] Dropping empty post-compression packet send.\n");
            fflush(stderr);
        }
        return;
    }

    uint8_t framed[8192];
    size_t framed_len = double_frame_packet(framed, sizeof(framed), packet, packet_len);

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

    if (packet == NULL || packet_len == 0) {
        if (g_log_packet_framing) {
            fprintf(stderr, "[WARN] Dropping empty large post-compression packet send.\n");
            fflush(stderr);
        }
        return;
    }

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

static void close_client_socket(socket_handle_t socket_fd) {
#ifdef _WIN32
    if (socket_fd == g_listen_socket) {
        fprintf(stderr, "[BUG] Refused attempt to close listening socket from client path.\n");
        fflush(stderr);
        append_lifecycle_log("[BUG] Refused close of listening socket handle=%llu", (unsigned long long)socket_fd);
        return;
    }
#else
    if (socket_fd == g_listen_socket) {
        fprintf(stderr, "[BUG] Refused attempt to close listening socket from client path.\n");
        fflush(stderr);
        append_lifecycle_log("[BUG] Refused close of listening socket handle=%d", socket_fd);
        return;
    }
#endif

#ifdef _WIN32
    closesocket(socket_fd);
#else
    close(socket_fd);
#endif
}

static int parse_handshake_next_state(const uint8_t *buffer,
                                      size_t bytes_read,
                                      int32_t *out_protocol_version,
                                      int32_t *out_next_state) {
    const uint8_t *ptr = buffer;
    size_t buflen = bytes_read;
    int32_t packet_length = read_varint(&ptr, &buflen);
    int32_t packet_id = read_varint(&ptr, &buflen);

    if (buffer == NULL || out_protocol_version == NULL || out_next_state == NULL) {
        return 0;
    }

    if (packet_length <= 0 || packet_id < 0 || (size_t)packet_length > buflen) {
        *out_protocol_version = -1;
        *out_next_state = -1;
        return 0;
    }

    buflen = (size_t)packet_length;

    printf("Received packet: length=%d, id=%d\n", packet_length, packet_id);
    if (packet_id != 0x00) {
        *out_protocol_version = -1;
        *out_next_state = -1;
        return 0;
    }

    {
        int32_t protocol_version = read_varint(&ptr, &buflen);
        extern char *read_mc_string(const uint8_t **, size_t *);
        char *server_address = read_mc_string(&ptr, &buflen);
        if (protocol_version < 0 || server_address == NULL || buflen < 2) {
            free(server_address);
            *out_protocol_version = -1;
            *out_next_state = -1;
            return 0;
        }
        uint16_t server_port = (uint16_t)(ptr[0] << 8 | ptr[1]);
        ptr += 2;
        buflen -= 2;
        *out_protocol_version = protocol_version;
        *out_next_state = read_varint(&ptr, &buflen);
        if (*out_next_state < 0) {
            free(server_address);
            *out_protocol_version = -1;
            *out_next_state = -1;
            return 0;
        }
        printf("Handshake: proto=%d, addr=%s, port=%u, next_state=%d\n", protocol_version, server_address, server_port, *out_next_state);
        free(server_address);
    }

    return 1;
}

static void handle_status_state(socket_handle_t socket_fd, const server_config_t *server_config) {
    uint8_t status_buf[1024];
#ifdef _WIN32
    int status_bytes = recv(socket_fd, (char*)status_buf, sizeof(status_buf), 0);
#else
    ssize_t status_bytes = recv(socket_fd, status_buf, sizeof(status_buf), 0);
#endif
    if (status_bytes > 0) {
        const uint8_t *sptr = status_buf;
        size_t slen = (size_t)status_bytes;
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
            send(socket_fd, (const char*)outbuf, (int)outlen, 0);
#else
            send(socket_fd, outbuf, outlen, 0);
#endif
            printf("Sent status response to client.\n");
        }

#ifdef _WIN32
        int ping_bytes = recv(socket_fd, (char*)status_buf, sizeof(status_buf), 0);
#else
        ssize_t ping_bytes = recv(socket_fd, status_buf, sizeof(status_buf), 0);
#endif
        if (ping_bytes > 0) {
#ifdef _WIN32
            send(socket_fd, (const char*)status_buf, ping_bytes, 0);
#else
            send(socket_fd, status_buf, ping_bytes, 0);
#endif
            printf("Echoed ping packet to client.\n");
        }
    }
}

static void run_play_state_loop(client_session_t *session,
                                socket_handle_t socket_fd,
                                socket_handle_t server_fd,
                                int has_world_info,
                                int force_debug_spawn,
                                world_info_t *world_info) {
    // Allocate decompression buffer once, at function scope
    uint8_t *inflate_tmp = (uint8_t *)malloc(8192);
    if (!inflate_tmp) {
        fprintf(stderr, "Failed to allocate inflate_tmp buffer\n");
        return;
    }

#ifdef _WIN32
    {
        DWORD timeout_ms = 1000;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));
    }
#else
    {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
#endif

    {
        int entered_play_state = 1;
        int idle_timeout_triggered = 0;
        uint64_t keepalive_sent = 0;
        uint64_t play_packets_received = 0;
        uint64_t play_packets_parsed = 0;
        uint64_t play_packets_parse_failed = 0;
        increment_connected_play_sessions();
        register_play_chat_socket(socket_fd, session->username);
        if (ensure_initial_moderator(session->username)) {
            char mod_msg[128];
            snprintf(mod_msg,
                     sizeof(mod_msg),
                     "[MOD] %s is now a moderator (first player).",
                     session->username[0] ? session->username : "Player");
            broadcast_system_chat_message(mod_msg, 0);
            append_moderation_audit("SYSTEM", "AUTO_OP", session->username, "first-player bootstrap");
            (void)save_moderation_state();
        }
        {
            char join_msg[128];
            snprintf(join_msg,
                     sizeof(join_msg),
                     "[JOIN] %s joined the game",
                     session->username[0] ? session->username : "Player");
            broadcast_system_chat_message(join_msg, 0);
        }
        time_t last_keepalive = time(NULL);
        time_t last_client_activity = time(NULL);
        time_t last_entity_event_log = time(NULL);
        time_t last_entity_refill = time(NULL);
        double last_activity_x = 0.0;
        double last_activity_z = 0.0;
        int has_last_activity_position = 0;
        int64_t ka_id = 1;
        uint64_t tick = 0;  /* Game tick counter for deterministic entity updates */
        
        printf("Play session started: idle_timeout=%d seconds, keep_alive_interval=%d seconds\n",
               session->server_config.play_idle_timeout_seconds,
               session->server_config.keep_alive_interval_seconds);
        fflush(stdout);
        for (;;) {
            #ifdef _WIN32
            Sleep(50);  /* 50ms = 20 TPS (ticks per second) */
            #else
            usleep(50000);  /* 50ms = 20 TPS */
            #endif
            tick++;
            // Send Keep Alive on the configured interval.
            time_t now = time(NULL);
            if (now - last_keepalive >= session->server_config.keep_alive_interval_seconds) {
                uint8_t ka_buf[32];
                size_t ka_len = build_keep_alive_packet(ka_buf, sizeof(ka_buf), ka_id++);
                send_post_compression_packet(socket_fd, ka_buf, ka_len);
                keepalive_sent += 1;
                printf("Sent Keep Alive (id=%lld).\n", (long long)(ka_id - 1));
                fflush(stdout);
                last_keepalive = now;
            }

            if (has_last_activity_position &&
                now - session->last_broken_block_reconcile_at >= 1) {
                reconcile_broken_blocks_near_player(session, last_activity_x, last_activity_z);
                session->last_broken_block_reconcile_at = now;
            }

            if (session->server_config.log_entity_events &&
                now - last_entity_event_log >= 1 &&
                entity_manager_pending_count(&session->entity_manager) > 0) {
                log_and_clear_entity_events(&session->entity_manager);
                last_entity_event_log = now;
            }

            if (session->server_config.enable_experimental_entities &&
                session->mock_entity_a != 0 &&
                session->mock_entity_b != 0 &&
                tick % 1 == 0) {  /* Update every tick (20 TPS) */
                double anchor_x = has_last_activity_position ? last_activity_x : 0.5;
                double anchor_z = has_last_activity_position ? last_activity_z : 0.5;
                double phase = (tick % 1200) * 0.005236;  /* 1200 ticks = 60s, smooth cycle */
                double ax = anchor_x + 2.0 + sin(phase) * 0.75;
                double az = anchor_z + 1.0 + cos(phase) * 0.75;
                double bx = anchor_x - 2.0 + cos(phase * 0.9) * 0.75;
                double bz = anchor_z - 1.0 + sin(phase * 0.9) * 0.75;
                double ay = 0.0;
                double by = 0.0;

                /* Capture old positions before the registry is updated. */
                double old_ax = ax, old_ay = 0.0, old_az = az;
                double old_bx = bx, old_by = 0.0, old_bz = bz;
                {
                    int i;
                    for (i = 0; i < 512; ++i) {
                        entity_state_t *e = &session->entity_registry.entries[i];
                        if (e->active && e->entity_id == session->mock_entity_a) {
                            old_ax = e->x; old_ay = e->y; old_az = e->z;
                        } else if (e->active && e->entity_id == session->mock_entity_b) {
                            old_bx = e->x; old_by = e->y; old_bz = e->z;
                        }
                    }
                }

                ay = old_ay;
                by = old_by;

                (void)entity_registry_update_xyz(&session->entity_registry,
                                                 session->mock_entity_a,
                                                 ax,
                                                 ay,
                                                 az);
                (void)entity_registry_update_xyz(&session->entity_registry,
                                                 session->mock_entity_b,
                                                 bx,
                                                 by,
                                                 bz);

                if (session->server_config.enable_experimental_entity_packets &&
                    session->client_protocol_version == 774) {
                    int was_visible_a = session->mock_entity_a_visible;
                    int was_visible_b = session->mock_entity_b_visible;
                    int should_emit_a;
                    int should_emit_b;

                    reconcile_entity_visibility(session,
                                                socket_fd,
                                                session->mock_entity_a,
                                                5,
                                                1,
                                                &session->mock_entity_a_visible);
                    reconcile_entity_visibility(session,
                                                socket_fd,
                                                session->mock_entity_b,
                                                5,
                                                1,
                                                &session->mock_entity_b_visible);
                    reconcile_tracked_spawned_entities_visibility(session, socket_fd);

                    /* Skip movement deltas for newly-spawned or out-of-range entities. */
                    should_emit_a = session->mock_entity_a_visible && was_visible_a;
                    should_emit_b = session->mock_entity_b_visible && was_visible_b;

                    if (!should_emit_a && !session->mock_entity_a_visible) {
                        session->entity_stats_update_culled += 1;
                    }
                    if (!should_emit_b && !session->mock_entity_b_visible) {
                        session->entity_stats_update_culled += 1;
                    }

                    if (should_emit_a) {
                        (void)entity_manager_queue_update_xyz(&session->entity_registry,
                                                              &session->entity_manager,
                                                              session->mock_entity_a,
                                                              ax,
                                                              ay,
                                                              az);
                    }
                    if (should_emit_b) {
                        (void)entity_manager_queue_update_xyz(&session->entity_registry,
                                                              &session->entity_manager,
                                                              session->mock_entity_b,
                                                              bx,
                                                              by,
                                                              bz);
                    }

                    /* Delta encoding: (new - old) * 4096, clamped to int16_t range.
                     * Orbit is XZ only; Y does not change, so dy is always 0.
                     * If any axis exceeds ±8 blocks (±32767 units) use position sync instead. */
                    double da_raw_x = should_emit_a ? (ax - old_ax) * 4096.0 : 0.0;
                    double da_raw_y = 0.0;
                    double da_raw_z = should_emit_a ? (az - old_az) * 4096.0 : 0.0;
                    double db_raw_x = should_emit_b ? (bx - old_bx) * 4096.0 : 0.0;
                    double db_raw_y = 0.0;
                    double db_raw_z = should_emit_b ? (bz - old_bz) * 4096.0 : 0.0;

#define ENTITY_DELTA_FITS(v) ((v) >= -32767.0 && (v) <= 32767.0)
                    uint8_t mv_buf[64];
                    size_t mv_len;

                    if (should_emit_a) {
                        if (ENTITY_DELTA_FITS(da_raw_x) && ENTITY_DELTA_FITS(da_raw_y) && ENTITY_DELTA_FITS(da_raw_z)) {
                            mv_len = build_move_entity_pos_packet(mv_buf, sizeof(mv_buf),
                                session->mock_entity_a,
                                (int16_t)da_raw_x, (int16_t)da_raw_y, (int16_t)da_raw_z, 0);
                        } else {
                            mv_len = build_entity_position_sync_packet(mv_buf, sizeof(mv_buf),
                                session->mock_entity_a, ax, old_ay, az, 0.0, 0.0, 0.0, 0.0f, 0.0f, 0);
                        }
                        send_post_compression_packet(socket_fd, mv_buf, mv_len);
                            session->entity_stats_move_packets_sent += 1;
                    }

                    if (should_emit_b) {
                        if (ENTITY_DELTA_FITS(db_raw_x) && ENTITY_DELTA_FITS(db_raw_y) && ENTITY_DELTA_FITS(db_raw_z)) {
                            mv_len = build_move_entity_pos_packet(mv_buf, sizeof(mv_buf),
                                session->mock_entity_b,
                                (int16_t)db_raw_x, (int16_t)db_raw_y, (int16_t)db_raw_z, 0);
                        } else {
                            mv_len = build_entity_position_sync_packet(mv_buf, sizeof(mv_buf),
                                session->mock_entity_b, bx, old_by, bz, 0.0, 0.0, 0.0, 0.0f, 0.0f, 0);
                        }
                        send_post_compression_packet(socket_fd, mv_buf, mv_len);
                            session->entity_stats_move_packets_sent += 1;
                    }

                    /* Head Rotation (0x51) — make entities look at player
                     * yaw = -atan2(dx, dz) * 180 / PI, where dx/dz are deltas from entity to player */
                    {
                        double player_x = has_last_activity_position ? last_activity_x : 0.5;
                        double player_z = has_last_activity_position ? last_activity_z : 0.5;
                        double dx_a = player_x - ax;
                        double dz_a = player_z - az;
                        double head_yaw_a = -atan2(dx_a, dz_a) * 180.0 / 3.14159265359;
                        uint8_t hr_buf[16];
                        size_t hr_len;
                        if (head_yaw_a < 0.0) head_yaw_a += 360.0;  /* Ensure 0-360 range */
                        if (should_emit_a) {
                            hr_len = build_set_head_rotation_packet(hr_buf, sizeof(hr_buf),
                                                                    session->mock_entity_a, (float)head_yaw_a);
                            send_post_compression_packet(socket_fd, hr_buf, hr_len);
                            session->entity_stats_head_packets_sent += 1;
                        }

                        double dx_b = player_x - bx;
                        double dz_b = player_z - bz;
                        double head_yaw_b = -atan2(dx_b, dz_b) * 180.0 / 3.14159265359;
                        if (head_yaw_b < 0.0) head_yaw_b += 360.0;
                        if (should_emit_b) {
                            hr_len = build_set_head_rotation_packet(hr_buf, sizeof(hr_buf),
                                                                    session->mock_entity_b, (float)head_yaw_b);
                            send_post_compression_packet(socket_fd, hr_buf, hr_len);
                            session->entity_stats_head_packets_sent += 1;
                        }
                    }
#undef ENTITY_DELTA_FITS
                } else {
                    (void)entity_manager_queue_update_xyz(&session->entity_registry,
                                                          &session->entity_manager,
                                                          session->mock_entity_a,
                                                          ax,
                                                          ay,
                                                          az);
                    (void)entity_manager_queue_update_xyz(&session->entity_registry,
                                                          &session->entity_manager,
                                                          session->mock_entity_b,
                                                          bx,
                                                          by,
                                                          bz);
                }
            }

            if (session->server_config.enable_experimental_entities &&
                session->server_config.enable_experimental_entity_packets &&
                session->client_protocol_version == 774) {
                tick_tracked_mob_movement(session,
                                          socket_fd,
                                          tick,
                                          has_last_activity_position,
                                          last_activity_x,
                                          last_activity_z);
                enforce_tracked_entity_limits(session,
                                              socket_fd,
                                              now,
                                              has_last_activity_position,
                                              last_activity_x,
                                              last_activity_z);

                if (now - last_entity_refill >= session->server_config.entity_respawn_interval_seconds) {
                    size_t active_before_refill = count_tracked_roaming_mobs(session);
                    size_t spawned_by_refill;
                    size_t active_after_refill;
                    spawned_by_refill = refill_tracked_roaming_mobs(session,
                                                                    socket_fd,
                                                                    tick,
                                                                    has_last_activity_position,
                                                                    last_activity_x,
                                                                    last_activity_z);
                    active_after_refill = count_tracked_roaming_mobs(session);
                    if (session->server_config.log_play_session_summary &&
                        (spawned_by_refill > 0 || active_before_refill < (size_t)session->server_config.entity_target_active_mobs)) {
                        printf("[ENTITY_REFILL] active=%zu target=%d spawned=%zu after=%zu tracked=%zu cap=%d\n",
                               active_before_refill,
                               session->server_config.entity_target_active_mobs,
                               spawned_by_refill,
                               active_after_refill,
                               session->spawned_entity_count,
                               session->server_config.entity_max_tracked);
                        fflush(stdout);
                    }
                    last_entity_refill = now;
                }
            }

            if (session->server_config.play_idle_timeout_seconds > 0 &&
                now - last_client_activity >= session->server_config.play_idle_timeout_seconds) {
                printf("Closing idle play session after %d seconds without client traffic.\n",
                       session->server_config.play_idle_timeout_seconds);
                if (session->server_config.send_idle_disconnect_packet) {
                    int disconnect_packet_id = session->server_config.play_disconnect_packet_id;
                    int use_network_nbt_string = 0;

                    if (session->client_protocol_version == 774) {
                        // Protocol 774 expects kick_disconnect on 0x1D with NBT-encoded text.
                        if (disconnect_packet_id == 0x1A) {
                            disconnect_packet_id = 0x1D;
                        }
                        if (disconnect_packet_id == 0x1D) {
                            use_network_nbt_string = 1;
                        }
                    }

                    send_play_disconnect(socket_fd,
                                         disconnect_packet_id,
                                         session->server_config.idle_disconnect_reason,
                                         use_network_nbt_string);
                    printf("Sent play disconnect packet for idle timeout (id=0x%02X, format=%s).\n",
                           disconnect_packet_id,
                           use_network_nbt_string ? "network-nbt-string" : "mc-string-json");
                }
                idle_timeout_triggered = 1;
                break;
            }

            uint8_t play_buf[2048];
#ifdef _WIN32
            int play_bytes = recv(socket_fd, (char*)play_buf, sizeof(play_buf), 0);
            if (play_bytes <= 0) {
                if (WSAGetLastError() == WSAETIMEDOUT) continue;
                if (play_bytes == 0) {
                    printf("[RECV] Socket closed by client (play_bytes=0)\n");
                } else {
                    printf("[RECV] Socket error: WSAGetLastError()=%d\n", WSAGetLastError());
                }
                break;
            }
#else
            ssize_t play_bytes = recv(socket_fd, play_buf, sizeof(play_buf), 0);
            printf("[RECV] recv() returned %zd\n", play_bytes);
            fflush(stdout);
            if (play_bytes <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (play_bytes == 0) {
                    printf("[RECV] Socket closed by client (play_bytes=0)\n");
                } else {
                    printf("[RECV] Socket error: errno=%d (%s)\n", errno, strerror(errno));
                }
                break;
            }
#endif
            play_packets_received += 1;
            {
                int32_t play_pid = -1;
                const uint8_t *play_payload = NULL;
                size_t play_payload_len = 0;

                if (read_post_compression_packet_data(
                        play_buf,
                        (size_t)play_bytes,
                        &play_pid,
                        &play_payload,
                        &play_payload_len,
                        inflate_tmp,
                        8192)) {
                    int counts_as_activity = 1;
                    int has_position = 0;
                    double player_x = 0.0;
                    double player_y = 0.0;
                    double player_z = 0.0;
                    play_packets_parsed += 1;
                    if (!session->server_config.idle_timeout_counts_keep_alive &&
                        play_pid == session->server_config.serverbound_keep_alive_packet_id) {
                        counts_as_activity = 0;
                        if (g_log_play_packets) {
                            printf("Ignoring keep-alive reply for idle timeout (packet ID %d).\n", play_pid);
                        }
                    }

                    has_position = try_extract_position_from_play_packet(
                        play_pid,
                        play_payload,
                        play_payload_len,
                        &player_x,
                        &player_y,
                        &player_z);

                    if (session->server_config.idle_timeout_requires_position_change) {
                        if (has_position) {
                            if (has_last_activity_position) {
                                double dx = fabs(player_x - last_activity_x);
                                double dz = fabs(player_z - last_activity_z);
                                double epsilon = (double)session->server_config.idle_position_epsilon_milliblocks / 1000.0;
                                if (dx <= epsilon && dz <= epsilon) {
                                    counts_as_activity = 0;
                                }
                            }
                        } else {
                            counts_as_activity = 0;
                        }
                    }

                    if (counts_as_activity) {
                        last_client_activity = time(NULL);
                        if (has_position) {
                            last_activity_x = player_x;
                            last_activity_z = player_z;
                            has_last_activity_position = 1;
                        }
                    }

                    if (has_position) {
                        (void)entity_manager_queue_update_xyz(&session->entity_registry,
                                                              &session->entity_manager,
                                                              1,
                                                              player_x,
                                                              player_y,
                                                              player_z);

                        {
                            time_t reconcile_now = time(NULL);
                            if (reconcile_now - session->last_broken_block_reconcile_at >= 1) {
                                reconcile_broken_blocks_near_player(session, player_x, player_z);
                                session->last_broken_block_reconcile_at = reconcile_now;
                            }
                        }

                        /* Auto-pickup for dropped item entities near the player. */
                        {
                            const double pickup_radius_sq = 2.25; /* 1.5 blocks */
                            size_t i = 0;
                            while (i < session->spawned_entity_count) {
                                int32_t ent_id = session->spawned_entity_ids[i];
                                int32_t ent_type = session->spawned_entity_types[i];
                                double ex = 0.0, ey = 0.0, ez = 0.0;

                                if (ent_type != PLAY774_ENTITY_TYPE_ITEM) {
                                    i += 1;
                                    continue;
                                }

                                if (!find_entity_position(&session->entity_registry, ent_id, &ex, &ey, &ez)) {
                                    (void)untrack_spawned_entity_id(session, ent_id);
                                    continue;
                                }

                                {
                                    double dx = player_x - ex;
                                    double dy = player_y - ey;
                                    double dz = player_z - ez;
                                    double dist_sq = dx * dx + dy * dy + dz * dz;

                                    if (dist_sq <= pickup_radius_sq) {
                                        uint8_t take_buf[64];
                                        size_t take_len = build_take_item_entity_packet(take_buf,
                                                                                        sizeof(take_buf),
                                                                                        ent_id,
                                                                                        1,
                                                                                        1);
                                        uint8_t rem_buf[64];
                                        size_t rem_len = build_entity_destroy_packet(rem_buf,
                                                                                     sizeof(rem_buf),
                                                                                     &ent_id,
                                                                                     1);
                                        send_post_compression_packet(socket_fd, take_buf, take_len);
                                        send_post_compression_packet(socket_fd, rem_buf, rem_len);
                                        session->entity_stats_destroy_packets_sent += 1;
                                        (void)entity_manager_queue_remove(&session->entity_registry,
                                                                          &session->entity_manager,
                                                                          ent_id);
                                        (void)untrack_spawned_entity_id(session, ent_id);
                                        continue;
                                    }
                                }

                                i += 1;
                            }
                        }
                    }

                    if (play_pid == PLAY774_C2S_BLOCK_DIG) {
                        int parsed_dig = 0;
                        int32_t dig_status = -1;
                        int32_t block_x = 0, block_y = 0, block_z = 0;
                        int32_t dig_sequence = -1;
                        time_t break_ts = time(NULL);

                            parsed_dig = try_parse_block_dig_packet_774(play_payload,
                                                                         play_payload_len,
                                                                         &dig_status,
                                                                         &block_x,
                                                                         &block_y,
                                                                         &block_z,
                                                                         &dig_sequence);
                            if (parsed_dig) {
                            uint8_t ack_buf[32];
                            size_t ack_len = build_ack_block_change_packet(ack_buf,
                                                                            sizeof(ack_buf),
                                                                            dig_sequence);
                            send_post_compression_packet(socket_fd, ack_buf, ack_len);
                        }

                            if (parsed_dig &&
                               (dig_status == PLAY774_BLOCK_DIG_FINISH ||
                                (dig_status == PLAY774_BLOCK_DIG_START && session->server_config.game_mode == 1))) {
                            int32_t drop_entity_id = 0;
                            double drop_x = (double)block_x + 0.5;
                            double drop_y = (double)block_y + 0.5;
                            double drop_z = (double)block_z + 0.5;
                            gen_block_kind_t broken_kind = generated_world_block_kind(block_x, block_y, block_z);

                            if (block_y < -64 || block_y > 319) {
                                goto block_dig_done;
                            }

                            if (is_block_marked_broken(session, block_x, block_y, block_z)) {
                                uint8_t bu_buf[32];
                                size_t bu_len = build_block_update_packet(bu_buf,
                                                                          sizeof(bu_buf),
                                                                          block_x,
                                                                          block_y,
                                                                          block_z,
                                                                          PLAY774_BLOCK_STATE_AIR);
                                send_post_compression_packet(socket_fd, bu_buf, bu_len);
                                goto block_dig_done;
                            }

                            mark_block_broken(session, block_x, block_y, block_z, break_ts);

                            {
                                uint8_t bu_buf[32];
                                size_t bu_len = build_block_update_packet(bu_buf,
                                                                          sizeof(bu_buf),
                                                                          block_x,
                                                                          block_y,
                                                                          block_z,
                                                                          PLAY774_BLOCK_STATE_AIR);
                                send_post_compression_packet(socket_fd, bu_buf, bu_len);
                            }

                            if (should_spawn_block_drop(session,
                                                        block_x,
                                                        block_y,
                                                        block_z,
                                                        break_ts) &&
                                ensure_tracked_slot_for_drop(session, socket_fd) &&
                                entity_manager_queue_spawn_auto(&session->entity_registry,
                                                                &session->entity_manager,
                                                                ENTITY_KIND_OBJECT,
                                                                drop_x,
                                                                drop_y,
                                                                drop_z,
                                                                &drop_entity_id)) {
                                uint8_t se_buf[128];
                                size_t se_len;

                                if (!track_spawned_entity_id(session,
                                                             drop_entity_id,
                                                             PLAY774_ENTITY_TYPE_ITEM)) {
                                    (void)entity_manager_queue_remove(&session->entity_registry,
                                                                      &session->entity_manager,
                                                                      drop_entity_id);
                                    goto block_dig_done;
                                }

                                se_len = build_spawn_entity_packet(se_buf,
                                                                   sizeof(se_buf),
                                                                   drop_entity_id,
                                                                   PLAY774_ENTITY_TYPE_ITEM,
                                                                   drop_x,
                                                                   drop_y,
                                                                   drop_z,
                                                                   0,
                                                                   0,
                                                                   0,
                                                                   0);
                                send_post_compression_packet(socket_fd, se_buf, se_len);
                                {
                                    uint8_t item_md_buf[64];
                                    int32_t item_id = PLAY774_ITEM_ID_STONE;
                                    if (broken_kind == GEN_BLOCK_SAND) {
                                        item_id = 93; /* minecraft:sand */
                                    }
                                    size_t item_md_len = build_set_item_entity_slot_packet(item_md_buf,
                                                                                           sizeof(item_md_buf),
                                                                                           drop_entity_id,
                                                                                           item_id,
                                                                                           1);
                                    if (item_md_len > 0) {
                                        send_post_compression_packet(socket_fd, item_md_buf, item_md_len);
                                    }
                                }
                                session->entity_stats_spawn_packets_sent += 1;
                            }
block_dig_done:
                            ;
                        }
                    }

                    if (g_log_play_packets) {
                        printf("Play packet ID from client: %d\n", play_pid);
                    }

                    if (session->client_protocol_version == 774) {
                        char command_text[256];
                        int have_command_text = 0;
                        const uint8_t *cmd_payload = play_payload;
                        size_t cmd_payload_len = play_payload_len;

                            /* Protocol 774 serverbound command/message packets.
                            * 0x06: chat_command
                            * 0x07: chat_command_signed
                            * 0x08: chat message */
                            if ((play_pid == PLAY774_C2S_CHAT_COMMAND ||
                                play_pid == PLAY774_C2S_CHAT_COMMAND_SIGNED ||
                                play_pid == PLAY774_C2S_CHAT_MESSAGE) &&
                            read_prefixed_string_copy(&cmd_payload,
                                                      &cmd_payload_len,
                                                      command_text,
                                                      sizeof(command_text))) {
                            have_command_text = 1;
                        }

                        if (have_command_text) {
                            char entity_name[64];
                            int spawn_count = 1;
                            int32_t entity_type = 0;
                            int handled = 0;
                            int sender_is_mod = is_username_moderator(session->username);

                            entity_name[0] = '\0';

                            if (command_equals(command_text, "help")) {
                                send_system_chat_message(socket_fd, "Commands: /spawn <type> [count] [radius] [y_offset] [glow|noglow]", 0);
                                send_system_chat_message(socket_fd, "Commands: /despawn <entity_id>, /despawnall, /listentities", 0);
                                send_system_chat_message(socket_fd, "Commands: /entitystats [reset] (entity visibility/update counters)", 0);
                                send_system_chat_message(socket_fd, "Commands: /mute <player>, /unmute <player>, /mutelist", 0);
                                send_system_chat_message(socket_fd, "Commands: /op <player>, /deop <player>, /modlist", 0);
                                send_system_chat_message(socket_fd, "Commands: /modlog [count]  (show last audit log entries, mod-only)", 0);
                                send_system_chat_message(socket_fd, "Examples: /spawn cow 5 4 0 glow ; /despawnall", 0);
                                handled = 1;
                            } else if (command_equals(command_text, "listentities")) {
                                if (session->spawned_entity_count == 0) {
                                    send_system_chat_message(socket_fd, "No tracked spawned entities.", 0);
                                } else {
                                    char feedback[192];
                                    size_t i;
                                    snprintf(feedback, sizeof(feedback), "Tracked entities: %zu", session->spawned_entity_count);
                                    send_system_chat_message(socket_fd, feedback, 0);
                                    for (i = 0; i < session->spawned_entity_count && i < 20; ++i) {
                                        int32_t id = session->spawned_entity_ids[i];
                                        int32_t typ = session->spawned_entity_types[i];
                                        double ex = 0.0, ey = 0.0, ez = 0.0;
                                        const char *name = lookup_entity_name_by_type_774(typ);
                                        if (find_entity_position(&session->entity_registry, id, &ex, &ey, &ez)) {
                                            snprintf(feedback, sizeof(feedback), "#%d %s(type=%d) at %.1f %.1f %.1f", (int)id, name, (int)typ, ex, ey, ez);
                                        } else {
                                            snprintf(feedback, sizeof(feedback), "#%d %s(type=%d) position unavailable", (int)id, name, (int)typ);
                                        }
                                        send_system_chat_message(socket_fd, feedback, 0);
                                    }
                                    if (session->spawned_entity_count > 20) {
                                        send_system_chat_message(socket_fd, "Showing first 20 tracked entities.", 0);
                                    }
                                }
                                handled = 1;
                            } else if (command_equals(command_text, "entitystats") ||
                                       command_extract_single_arg(command_text, "entitystats", entity_name, sizeof(entity_name))) {
                                char feedback[224];
                                if (strcmp(entity_name, "reset") == 0) {
                                    session->entity_stats_spawn_packets_sent = 0;
                                    session->entity_stats_destroy_packets_sent = 0;
                                    session->entity_stats_move_packets_sent = 0;
                                    session->entity_stats_head_packets_sent = 0;
                                    session->entity_stats_update_culled = 0;
                                    send_system_chat_message(socket_fd, "[ENTITY] Counters reset.", 0);
                                    handled = 1;
                                } else if (entity_name[0] != '\0' && !command_equals(command_text, "entitystats")) {
                                    send_system_chat_message(socket_fd, "[ENTITY] Usage: /entitystats [reset]", 0);
                                    handled = 1;
                                }

                                if (!handled) {
                                snprintf(feedback,
                                         sizeof(feedback),
                                         "[ENTITY] tracked=%zu spawn=%llu destroy=%llu move=%llu head=%llu culled=%llu",
                                         session->spawned_entity_count,
                                         (unsigned long long)session->entity_stats_spawn_packets_sent,
                                         (unsigned long long)session->entity_stats_destroy_packets_sent,
                                         (unsigned long long)session->entity_stats_move_packets_sent,
                                         (unsigned long long)session->entity_stats_head_packets_sent,
                                         (unsigned long long)session->entity_stats_update_culled);
                                send_system_chat_message(socket_fd, feedback, 0);
                                handled = 1;
                                }
                            } else {
                                char target_name[32];
                                if (command_extract_single_arg(command_text, "mute", target_name, sizeof(target_name))) {
                                    char feedback[160];
                                    if (!sender_is_mod) {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] You do not have permission to use /mute.");
                                    } else if (add_muted_username(target_name)) {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] Muted %s.",
                                                 target_name);
                                        append_moderation_audit(session->username, "MUTE", target_name, "ok");
                                        (void)save_moderation_state();
                                    } else {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] %s is already muted or mute list is full.",
                                                 target_name);
                                    }
                                    send_system_chat_message(socket_fd, feedback, 0);
                                    handled = 1;
                                } else if (command_extract_single_arg(command_text, "unmute", target_name, sizeof(target_name))) {
                                    char feedback[160];
                                    if (!sender_is_mod) {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] You do not have permission to use /unmute.");
                                    } else if (remove_muted_username(target_name)) {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] Unmuted %s.",
                                                 target_name);
                                        append_moderation_audit(session->username, "UNMUTE", target_name, "ok");
                                        (void)save_moderation_state();
                                    } else {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] %s was not muted.",
                                                 target_name);
                                    }
                                    send_system_chat_message(socket_fd, feedback, 0);
                                    handled = 1;
                                } else if (command_equals(command_text, "mutelist")) {
                                    char muted_snapshot[20][32];
                                    size_t muted_count = 0;
                                    size_t i;
                                    PLAY_CHAT_LOCK();
                                    muted_count = g_muted_username_count;
                                    if (muted_count > 20) {
                                        muted_count = 20;
                                    }
                                    for (i = 0; i < muted_count; ++i) {
                                        snprintf(muted_snapshot[i], sizeof(muted_snapshot[i]), "%s", g_muted_usernames[i]);
                                    }
                                    PLAY_CHAT_UNLOCK();

                                    if (muted_count == 0) {
                                        send_system_chat_message(socket_fd, "[MOD] No muted players.", 0);
                                    } else {
                                        char feedback[192];
                                        send_system_chat_message(socket_fd, "[MOD] Muted players:", 0);
                                        for (i = 0; i < muted_count; ++i) {
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "- %s",
                                                     muted_snapshot[i]);
                                            send_system_chat_message(socket_fd, feedback, 0);
                                        }
                                        if (muted_count == 20) {
                                            send_system_chat_message(socket_fd, "[MOD] Showing first 20 muted players.", 0);
                                        }
                                    }
                                    handled = 1;
                                } else if (command_extract_single_arg(command_text, "op", target_name, sizeof(target_name))) {
                                    char feedback[160];
                                    if (!sender_is_mod) {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] You do not have permission to use /op.");
                                    } else if (add_moderator_username(target_name)) {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] %s is now a moderator.",
                                                 target_name);
                                        broadcast_system_chat_message(feedback, 0);
                                        append_moderation_audit(session->username, "OP", target_name, "ok");
                                        (void)save_moderation_state();
                                    } else {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] %s is already a moderator or moderator list is full.",
                                                 target_name);
                                    }
                                    send_system_chat_message(socket_fd, feedback, 0);
                                    handled = 1;
                                } else if (command_extract_single_arg(command_text, "deop", target_name, sizeof(target_name))) {
                                    char feedback[160];
                                    int rc;
                                    if (!sender_is_mod) {
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "[MOD] You do not have permission to use /deop.");
                                    } else {
                                        rc = remove_moderator_username(target_name);
                                        if (rc == 1) {
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "[MOD] %s is no longer a moderator.",
                                                     target_name);
                                            broadcast_system_chat_message(feedback, 0);
                                            append_moderation_audit(session->username, "DEOP", target_name, "ok");
                                            (void)save_moderation_state();
                                        } else if (rc == -1) {
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "[MOD] Cannot remove the last moderator.");
                                        } else {
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "[MOD] %s is not a moderator.",
                                                     target_name);
                                        }
                                    }
                                    send_system_chat_message(socket_fd, feedback, 0);
                                    handled = 1;
                                } else if (command_equals(command_text, "modlist")) {
                                    char moderator_snapshot[20][32];
                                    size_t moderator_count = 0;
                                    size_t i;
                                    PLAY_CHAT_LOCK();
                                    moderator_count = g_moderator_username_count;
                                    if (moderator_count > 20) {
                                        moderator_count = 20;
                                    }
                                    for (i = 0; i < moderator_count; ++i) {
                                        snprintf(moderator_snapshot[i], sizeof(moderator_snapshot[i]), "%s", g_moderator_usernames[i]);
                                    }
                                    PLAY_CHAT_UNLOCK();

                                    if (moderator_count == 0) {
                                        send_system_chat_message(socket_fd, "[MOD] No moderators configured.", 0);
                                    } else {
                                        char feedback[192];
                                        send_system_chat_message(socket_fd, "[MOD] Moderators:", 0);
                                        for (i = 0; i < moderator_count; ++i) {
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "- %s",
                                                     moderator_snapshot[i]);
                                            send_system_chat_message(socket_fd, feedback, 0);
                                        }
                                        if (moderator_count == 20) {
                                            send_system_chat_message(socket_fd, "[MOD] Showing first 20 moderators.", 0);
                                        }
                                    }
                                    handled = 1;
                                } else if (command_equals(command_text, "modlog") ||
                                           command_extract_single_arg(command_text, "modlog", target_name, sizeof(target_name))) {
                                    if (!sender_is_mod) {
                                        send_system_chat_message(socket_fd, "[MOD] You do not have permission to use /modlog.", 0);
                                    } else {
                                        int log_count = 10;
                                        /* target_name holds the optional numeric arg if any */
                                        if (target_name[0] != '\0') {
                                            int parsed = 0;
                                            if (sscanf(target_name, "%d", &parsed) == 1 && parsed > 0) {
                                                log_count = parsed;
                                            }
                                        }
                                        send_modlog_to_player(socket_fd, log_count);
                                    }
                                    handled = 1;
                                }
                            }

                            if (!handled) {
                                double spawn_radius = 2.0;
                                double spawn_y_offset = 0.0;
                                int spawn_glow = 0;

                                if (try_parse_spawn_request_ex(command_text,
                                                               entity_name,
                                                               sizeof(entity_name),
                                                               &spawn_count,
                                                               &spawn_radius,
                                                               &spawn_y_offset,
                                                               &spawn_glow)) {
                                    if (lookup_entity_type_774(entity_name, &entity_type)) {
                                        double spawn_base_x = has_last_activity_position ? last_activity_x : 0.5;
                                        double spawn_base_z = has_last_activity_position ? last_activity_z : 0.5;
                                        double spawn_base_y = 64.0;
                                        int spawned_ok = 0;
                                        int i;

                                        for (i = 0; i < 512; ++i) {
                                            entity_state_t *e = &session->entity_registry.entries[i];
                                            if (e->active && e->entity_id == 1) {
                                                spawn_base_y = e->y;
                                                break;
                                            }
                                        }

                                        for (i = 0; i < spawn_count; ++i) {
                                            int32_t new_entity_id = 0;
                                            int tracked_idx = -1;
                                            double angle = ((double)i / (double)spawn_count) * 6.28318530718;
                                            double sx = spawn_base_x + cos(angle) * spawn_radius;
                                            double sy = spawn_base_y + spawn_y_offset;
                                            double sz = spawn_base_z + sin(angle) * spawn_radius;
                                            uint8_t se_buf[128];
                                            size_t se_len;

                                            if (!entity_manager_queue_spawn_auto(&session->entity_registry,
                                                                                 &session->entity_manager,
                                                                                 ENTITY_KIND_MOB,
                                                                                 sx,
                                                                                 sy,
                                                                                 sz,
                                                                                 &new_entity_id)) {
                                                continue;
                                            }

                                            if (track_spawned_entity_id(session, new_entity_id, entity_type)) {
                                                tracked_idx = find_tracked_spawned_entity_index(session, new_entity_id);
                                            }

                                            if (tracked_idx >= 0) {
                                                session->spawned_entity_visible[tracked_idx] = 0;
                                                reconcile_entity_visibility(session,
                                                                            socket_fd,
                                                                            new_entity_id,
                                                                            entity_type,
                                                                            spawn_glow,
                                                                            &session->spawned_entity_visible[tracked_idx]);
                                            } else {
                                                /* Fallback if tracking list is full. */
                                                se_len = build_spawn_entity_packet(se_buf,
                                                                                   sizeof(se_buf),
                                                                                   new_entity_id,
                                                                                   entity_type,
                                                                                   sx,
                                                                                   sy,
                                                                                   sz,
                                                                                   0,
                                                                                   0,
                                                                                   0,
                                                                                   0);
                                                send_post_compression_packet(socket_fd, se_buf, se_len);
                                                session->entity_stats_spawn_packets_sent += 1;
                                                if (entity_type == PLAY774_ENTITY_TYPE_ITEM) {
                                                    uint8_t item_md_buf[64];
                                                    size_t item_md_len = build_set_item_entity_slot_packet(item_md_buf,
                                                                                                           sizeof(item_md_buf),
                                                                                                           new_entity_id,
                                                                                                           PLAY774_ITEM_ID_STONE,
                                                                                                           1);
                                                    if (item_md_len > 0) {
                                                        send_post_compression_packet(socket_fd, item_md_buf, item_md_len);
                                                    }
                                                }
                                                if (spawn_glow) {
                                                    uint8_t md_buf[32];
                                                    size_t md_len = build_set_entity_glowing_packet(md_buf, sizeof(md_buf), new_entity_id, 1);
                                                    send_post_compression_packet(socket_fd, md_buf, md_len);
                                                }
                                            }
                                            spawned_ok += 1;
                                        }

                                        printf("Spawn command '%s': spawned %d entity(s) of type %s (%d).\n",
                                               command_text,
                                               spawned_ok,
                                               entity_name,
                                               entity_type);
                                        {
                                            char feedback[256];
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "Spawned %d %s (r=%.1f yoff=%.1f glow=%s).",
                                                     spawned_ok,
                                                     entity_name,
                                                     spawn_radius,
                                                     spawn_y_offset,
                                                     spawn_glow ? "yes" : "no");
                                            send_system_chat_message(socket_fd, feedback, 0);
                                        }
                                    } else {
                                        printf("Spawn command '%s' ignored: unknown entity '%s'.\n",
                                               command_text,
                                               entity_name);
                                        {
                                            char feedback[256];
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "Unknown entity '%s'. Try: cow pig sheep zombie skeleton creeper spider enderman villager horse cat wolf rabbit chicken.",
                                                     entity_name);
                                            send_system_chat_message(socket_fd, feedback, 0);
                                        }
                                    }
                                } else {
                                    int remove_all = 0;
                                    int32_t remove_id = 0;
                                    if (try_parse_despawn_request(command_text, &remove_all, &remove_id)) {
                                        if (remove_all) {
                                            int32_t remove_ids[256];
                                            size_t remove_count = 0;
                                            size_t i;

                                            for (i = 0; i < session->spawned_entity_count; ++i) {
                                                int32_t id = session->spawned_entity_ids[i];
                                                if (entity_manager_queue_remove(&session->entity_registry,
                                                                                &session->entity_manager,
                                                                                id)) {
                                                    if (session->spawned_entity_visible[i]) {
                                                        remove_ids[remove_count++] = id;
                                                    }
                                                }
                                            }

                                            session->spawned_entity_count = 0;

                                            if (remove_count > 0) {
                                                uint8_t rem_buf[2048];
                                                size_t rem_len = build_entity_destroy_packet(rem_buf,
                                                                                             sizeof(rem_buf),
                                                                                             remove_ids,
                                                                                             remove_count);
                                                send_post_compression_packet(socket_fd, rem_buf, rem_len);
                                                session->entity_stats_destroy_packets_sent += remove_count;
                                            }

                                            {
                                                char feedback[128];
                                                snprintf(feedback,
                                                         sizeof(feedback),
                                                         "Despawned %zu tracked entity(s).",
                                                         remove_count);
                                                send_system_chat_message(socket_fd, feedback, 0);
                                            }
                                        } else {
                                            int removed = 0;
                                            int was_visible = 0;
                                            if (is_tracked_spawned_entity_id(session, remove_id)) {
                                                int tracked_idx = find_tracked_spawned_entity_index(session, remove_id);
                                                if (tracked_idx >= 0) {
                                                    was_visible = session->spawned_entity_visible[tracked_idx];
                                                }
                                                removed = entity_manager_queue_remove(&session->entity_registry,
                                                                                      &session->entity_manager,
                                                                                      remove_id);
                                                if (removed) {
                                                    if (was_visible) {
                                                        uint8_t rem_buf[64];
                                                        size_t rem_len = build_entity_destroy_packet(rem_buf,
                                                                                                     sizeof(rem_buf),
                                                                                                     &remove_id,
                                                                                                     1);
                                                        send_post_compression_packet(socket_fd, rem_buf, rem_len);
                                                        session->entity_stats_destroy_packets_sent += 1;
                                                    }
                                                    (void)untrack_spawned_entity_id(session, remove_id);
                                                }
                                            }

                                            if (removed) {
                                                char feedback[128];
                                                snprintf(feedback,
                                                         sizeof(feedback),
                                                         "Despawned entity %d.",
                                                         (int)remove_id);
                                                send_system_chat_message(socket_fd, feedback, 0);
                                            } else {
                                                char feedback[160];
                                                snprintf(feedback,
                                                         sizeof(feedback),
                                                         "Entity %d not found in tracked spawned entities.",
                                                         (int)remove_id);
                                                send_system_chat_message(socket_fd, feedback, 0);
                                            }
                                        }
                                    } else if (play_pid == PLAY774_C2S_CHAT_MESSAGE) {
                                        const char *sender_name = session->username[0] ? session->username : "Player";
                                        int block_chat = 0;
                                        if (is_username_muted(sender_name)) {
                                            send_system_chat_message(socket_fd, "[MOD] You are muted.", 0);
                                            block_chat = 1;
                                        } else {
                                            int cooldown_left = 0;
                                            if (!record_chat_message_and_check_limit(session, time(NULL), &cooldown_left)) {
                                                char feedback[160];
                                                snprintf(feedback,
                                                         sizeof(feedback),
                                                         "[MOD] Chat slow mode: wait %d second(s).",
                                                         cooldown_left > 0 ? cooldown_left : 1);
                                                send_system_chat_message(socket_fd, feedback, 0);
                                                if (session->chat_rate_strike_count >= 3) {
                                                    char alert[224];
                                                    snprintf(alert,
                                                             sizeof(alert),
                                                             "[MOD ALERT] %s hit spam strike %d (cooldown %ds).",
                                                             sender_name,
                                                             session->chat_rate_strike_count,
                                                             cooldown_left > 0 ? cooldown_left : 1);
                                                    broadcast_moderator_system_message(alert, 0);
                                                }
                                                block_chat = 1;
                                            }
                                        }

                                        if (!block_chat) {
                                            char feedback[320];
                                            snprintf(feedback,
                                                     sizeof(feedback),
                                                     "[CHAT] <%s> %s",
                                                     sender_name,
                                                     command_text);
                                            broadcast_system_chat_message(feedback, 0);
                                        }
                                        handled = 1;
                                    } else if (play_pid == PLAY774_C2S_CHAT_COMMAND ||
                                               play_pid == PLAY774_C2S_CHAT_COMMAND_SIGNED) {
                                        char feedback[224];
                                        snprintf(feedback,
                                                 sizeof(feedback),
                                                 "Unknown command: /%s (try /help)",
                                                 command_text);
                                        send_system_chat_message(socket_fd, feedback, 0);
                                    }
                                }
                            }
                        }
                    }

                    {
                        if (has_position) {
                            int32_t moved_chunk_x = (int32_t)floor(player_x / 16.0);
                            int32_t moved_chunk_z = (int32_t)floor(player_z / 16.0);

                            if (moved_chunk_x != session->stream_state.stream_center_chunk_x ||
                                moved_chunk_z != session->stream_state.stream_center_chunk_z) {
                                uint8_t center_buf[64];
                                size_t center_len = build_set_center_chunk_packet(
                                    center_buf,
                                    sizeof(center_buf),
                                    moved_chunk_x,
                                    moved_chunk_z);
                                send_post_compression_packet(socket_fd, center_buf, center_len);

                                {
                                    uint8_t cbs_buf[4];
                                    size_t cbs_len = 0;
                                    int sent_chunks = 0;
                                    uint8_t cbf_buf[8];
                                    size_t cbf_len = 0;

                                    cbs_len += write_varint(cbs_buf + cbs_len, 0x0C);
                                    send_post_compression_packet(socket_fd, cbs_buf, cbs_len);

                                    for (int dz = -session->stream_state.chunk_stream_radius; dz <= session->stream_state.chunk_stream_radius; dz++) {
                                        for (int dx = -session->stream_state.chunk_stream_radius; dx <= session->stream_state.chunk_stream_radius; dx++) {
                                            int32_t sx = moved_chunk_x + dx;
                                            int32_t sz = moved_chunk_z + dz;
                                            int already_loaded =
                                                abs(sx - session->stream_state.stream_center_chunk_x) <= session->stream_state.chunk_stream_radius &&
                                                abs(sz - session->stream_state.stream_center_chunk_z) <= session->stream_state.chunk_stream_radius;

                                            if (already_loaded) {
                                                continue;
                                            }

                                            if (send_stream_chunk(session, sx, sz, world_info, NULL)) {
                                                sent_chunks += 1;
                                            }
                                        }
                                    }

                                    cbf_len += write_varint(cbf_buf + cbf_len, 0x0B);
                                    cbf_len += write_varint(cbf_buf + cbf_len, sent_chunks);
                                    send_post_compression_packet(socket_fd, cbf_buf, cbf_len);
                                }

                                session->stream_state.stream_center_chunk_x = moved_chunk_x;
                                session->stream_state.stream_center_chunk_z = moved_chunk_z;

                                if (session->server_config.enable_experimental_entities &&
                                    session->server_config.enable_experimental_entity_packets &&
                                    session->client_protocol_version == 774) {
                                    reconcile_entity_visibility(session,
                                                                socket_fd,
                                                                session->mock_entity_a,
                                                                5,
                                                                1,
                                                                &session->mock_entity_a_visible);
                                    reconcile_entity_visibility(session,
                                                                socket_fd,
                                                                session->mock_entity_b,
                                                                5,
                                                                1,
                                                                &session->mock_entity_b_visible);
                                    reconcile_tracked_spawned_entities_visibility(session, socket_fd);
                                }
                            }
                        }
                    }
                } else {
                    play_packets_parse_failed += 1;
                    if (g_log_packet_framing) {
                        printf("Failed to parse play packet in post-compression format.\n");
                    }
                }
            }
        }

        (void)entity_manager_queue_remove(&session->entity_registry, &session->entity_manager, 1);
        if (session->mock_entity_a != 0) {
            (void)entity_manager_queue_remove(&session->entity_registry,
                                              &session->entity_manager,
                                              session->mock_entity_a);
        }
        if (session->mock_entity_b != 0) {
            (void)entity_manager_queue_remove(&session->entity_registry,
                                              &session->entity_manager,
                                              session->mock_entity_b);
        }

        if (session->server_config.enable_experimental_entity_packets &&
            session->client_protocol_version == 774 &&
            (session->mock_entity_a != 0 || session->mock_entity_b != 0)) {
            // Send Remove Entities (0x4B) for mock entities on disconnect.
            int32_t remove_ids[2];
            size_t remove_count = 0;
            if (session->mock_entity_a != 0) remove_ids[remove_count++] = session->mock_entity_a;
            if (session->mock_entity_b != 0) remove_ids[remove_count++] = session->mock_entity_b;
            uint8_t rem_buf[32];
            size_t rem_len = build_entity_destroy_packet(rem_buf, sizeof(rem_buf), remove_ids, remove_count);
            send_post_compression_packet(socket_fd, rem_buf, rem_len);
            printf("Sent Remove Entities for mock entities on disconnect.\n");
        }

        if (session->server_config.log_entity_events) {
            log_and_clear_entity_events(&session->entity_manager);
        }

        if (session->server_config.log_play_session_summary) {
            time_t session_now = time(NULL);
            int session_duration = (int)(session_now - last_client_activity) + 1;  // Rough estimate using client activity time
                 printf("Play session summary: username=%s, reason=%s, keepalives_sent=%llu, packets_received=%llu, packets_parsed=%llu, packets_parse_failed=%llu, tracked_entities=%zu, pending_entity_events=%zu, dropped_entity_events=%zu\n",
                   session->username,
                   idle_timeout_triggered ? "idle-timeout" : "socket-closed",
                   (unsigned long long)keepalive_sent,
                   (unsigned long long)play_packets_received,
                   (unsigned long long)play_packets_parsed,
                   (unsigned long long)play_packets_parse_failed,
                   entity_registry_count(&session->entity_registry),
                     entity_manager_pending_count(&session->entity_manager),
                     entity_manager_dropped_count(&session->entity_manager));
            fflush(stdout);
        }

        entity_manager_clear(&session->entity_manager);

        if (entered_play_state) {
            char leave_msg[128];
            snprintf(leave_msg,
                     sizeof(leave_msg),
                     "[LEAVE] %s left the game",
                     session->username[0] ? session->username : "Player");
            broadcast_system_chat_message(leave_msg, 0);
            unregister_play_chat_socket(socket_fd);
            decrement_connected_play_sessions();
        }
    }
    
    // Free the decompression buffer allocated at session start
    if (inflate_tmp) {
        free(inflate_tmp);
    }

    (void)server_fd;
    (void)has_world_info;
    (void)force_debug_spawn;
}

static void handle_client_connection(client_session_t *session) {
    socket_handle_t new_socket = session->socket_fd;
    socket_handle_t server_fd = session->server_fd;
    server_config_t server_config = session->server_config;

    // Read initial packet(s) - may contain both handshake and login start
    uint8_t buffer[1024];
#ifdef _WIN32
    int bytes_read = recv(new_socket, (char*)buffer, sizeof(buffer), 0);
#else
    ssize_t bytes_read = recv(new_socket, buffer, sizeof(buffer), 0);
#endif
    if (bytes_read <= 0) {
        close_client_socket(new_socket);
        return;
    }

    // Parse handshake and track consumed bytes
    {
        int32_t protocol_version = -1;
        int32_t next_state = -1;
        const uint8_t *hs_ptr = buffer;
        size_t hs_len = bytes_read;
        
        if (!parse_handshake_next_state(hs_ptr, hs_len, &protocol_version, &next_state)) {
            close_client_socket(new_socket);
            return;
        }

        session->client_protocol_version = protocol_version;
        printf("[HANDSHAKE] Parsed: proto=%d, next_state=%d\n", protocol_version, next_state);

        if (next_state == 1) {
            printf("[HANDSHAKE] next_state=1 (STATUS), calling handle_status_state\n");
            handle_status_state(new_socket, &server_config);
            printf("[HANDSHAKE] handle_status_state completed, closing socket\n");
            close_client_socket(new_socket);
            return;
        }

        if (next_state != 2) {
            printf("[HANDSHAKE] next_state=%d (invalid, expected 1 or 2), closing socket\n", next_state);
            close_client_socket(new_socket);
            return;
        }
        printf("[HANDSHAKE] next_state=2 (LOGIN), proceeding to login phase\n");
    }

    // Set recv timeout on socket for login phase
    {
#ifdef _WIN32
        DWORD timeout_ms = 5000;  // 5-second timeout
        setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));
#else
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    // Login Start packet may already be in buffer (if client sent handshake+login together)
    // So we need to parse from the buffer first, then recv() if needed
    uint8_t login_buf[1024];
#ifdef _WIN32
    int login_bytes = 0;
#else
    ssize_t login_bytes = 0;
#endif
    {
        // We need to properly skip the entire handshake packet
        // Handshake format: [VarInt length][VarInt protocol][VarInt addr_len][String addr][UShort port][VarInt next_state]
        const uint8_t *parse_ptr = buffer;
        size_t parse_len = bytes_read;
        
        // Skip handshake length varint
        int32_t hs_packet_len = read_varint(&parse_ptr, &parse_len);
        printf("[LOGIN] Handshake packet length: %d\n", hs_packet_len);

        if (hs_packet_len <= 0) {
            printf("[LOGIN] Invalid handshake packet length: %d\n", hs_packet_len);
            close_client_socket(new_socket);
            return;
        }
        
        // Skip handshake contents (protocol + address + port + next_state)
        // This is approximately hs_packet_len bytes
        if (parse_len >= (size_t)hs_packet_len) {
            parse_ptr += hs_packet_len;
            parse_len -= hs_packet_len;
            printf("[LOGIN] After skipping handshake, found %zd bytes remaining\n", parse_len);
        } else {
            printf("[LOGIN] WARNING: handshake length (%d) exceeds remaining buffer (%zd)\n", hs_packet_len, parse_len);
        }
        
        // Now parse_ptr should be at the start of Login Start packet
        if (parse_len > 0) {
            // We have data after the handshake - copy it to login_buf
            if (parse_len > sizeof(login_buf)) {
                parse_len = sizeof(login_buf);
            }
#ifdef _WIN32
            login_bytes = (parse_len > (size_t)INT_MAX) ? INT_MAX : (int)parse_len;
#else
            login_bytes = (ssize_t)parse_len;
#endif
            memcpy(login_buf, parse_ptr, parse_len);
            if (g_log_packet_framing) {
                printf("[LOGIN] Found %zd bytes after handshake (hex):", parse_len);
                for (size_t i = 0; i < (parse_len < 20 ? parse_len : 20); i++) printf(" %02X", parse_ptr[i]);
                if (parse_len > 20) printf(" ...");
                printf("\n");
            }
        }
    }

    // If we didn't find login start in buffer, recv() it now
    if (login_bytes <= 0) {
        printf("[LOGIN] No data after handshake, waiting for Login Start packet...\n");
#ifdef _WIN32
        login_bytes = recv(new_socket, (char*)login_buf, sizeof(login_buf), 0);
#else
        login_bytes = recv(new_socket, login_buf, sizeof(login_buf), 0);
#endif
        if (login_bytes <= 0) {
            if (login_bytes == 0) {
                printf("[LOGIN] Socket closed (login_bytes=0)\n");
            } else {
#ifdef _WIN32
                printf("[LOGIN] recv timeout/error: WSAGetLastError()=%d\n", WSAGetLastError());
#else
                printf("[LOGIN] recv timeout/error: errno=%d (%s)\n", errno, strerror(errno));
#endif
            }
            close_client_socket(new_socket);
            return;
        }
    }

    {
        const uint8_t *lptr = login_buf;
        size_t llen = login_bytes;
        if (g_log_packet_framing) {
            printf("[LOGIN] Parsing %zd bytes from login_buf (hex):", llen);
            for (size_t i = 0; i < (llen < 20 ? llen : 20); i++) printf(" %02X", lptr[i]);
            if (llen > 20) printf(" ...");
            printf("\n");
        }
        
        int32_t lplen = read_varint(&lptr, &llen);
        printf("[LOGIN] Packet length varint: %d, remaining: %zd\n", lplen, llen);

        if (lplen <= 0 || (size_t)lplen > llen) {
            printf("[LOGIN] ERROR: invalid login packet length %d (remaining=%zd)\n", lplen, llen);
            close_client_socket(new_socket);
            return;
        }

        llen = (size_t)lplen;
        
        int32_t lpid = read_varint(&lptr, &llen);
        printf("[LOGIN] Packet ID: 0x%02X, remaining: %zd\n", lpid, llen);
        (void)lplen;
        if (lpid != 0x00) {
            printf("[LOGIN] ERROR: Expected packet ID 0x00 (Login Start), got 0x%02X\n", lpid);
            close_client_socket(new_socket);
            return;
        }

        // Parse username (MC String)
        extern char *read_mc_string(const uint8_t **, size_t *);
        {
                char *username = read_mc_string(&lptr, &llen);
                if (!username) {
                    close_client_socket(new_socket);
                    return;
                }
                printf("Login Start: username=%s\n", username);
                snprintf(session->username, sizeof(session->username), "%s", username);

                if (server_config.reject_protocol_mismatch &&
                    session->client_protocol_version != server_config.protocol_number) {
                    char reason_json[256];
                    snprintf(reason_json,
                             sizeof(reason_json),
                             "{\"text\":\"Unsupported protocol %d. This server expects protocol %d.\"}",
                             session->client_protocol_version,
                             server_config.protocol_number);
                    send_login_disconnect(new_socket, reason_json);
                    printf("Rejected client due to protocol mismatch (client=%d, configured=%d).\n",
                           session->client_protocol_version,
                           server_config.protocol_number);
                    free(username);
                    close_client_socket(new_socket);
                    return;
                }


                // Send Set Compression packet (id 0x03, Login state)
                printf("Preparing to send Set Compression...\n");
                extern size_t build_set_compression_packet(uint8_t *, size_t, int);
                uint8_t comp_buf[16];
                size_t comp_buf_len = build_set_compression_packet(
                    comp_buf,
                    sizeof(comp_buf),
                    server_config.compression_threshold);
                if (g_log_packet_framing) {
                    printf("Set Compression packet (hex): ");
                    for (size_t i = 0; i < comp_buf_len; ++i) printf("%02X ", comp_buf[i]);
                    printf("\n");
                }

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
                extern void write_offline_mode_uuid(uint8_t *, const char *);
                write_offline_mode_uuid(uuid, username);
                if (g_log_packet_framing) {
                    printf("Login Success UUID bytes (offline-mode from username: %s): ", username);
                    for (size_t i = 0; i < sizeof(uuid); ++i) printf("%02X ", uuid[i]);
                    printf("\n");
                }

                // Encode username as MC String
                uint8_t unamebuf[64];
                extern size_t write_varint(uint8_t *, int32_t);
                size_t uname_len = strlen(username);
                size_t uname_varint = write_varint(unamebuf, (int32_t)uname_len);
                if (uname_len > 32 || uname_varint + uname_len > sizeof(unamebuf)) {
                    printf("[LOGIN] ERROR: Username too long for Login Success encoding (len=%zu).\n", uname_len);
                    free(username);
                    close_client_socket(new_socket);
                    return;
                }
                memcpy(unamebuf + uname_varint, username, uname_len);

                // Print username encoding for debug
                if (g_log_packet_framing) {
                    printf("Login Success username (len=%zu): ", uname_len);
                    for (size_t i = 0; i < uname_varint + uname_len; ++i) printf("%02X ", unamebuf[i]);
                    printf("\n");
                }


                // Build Login Success packet (raw, no length prefix)
                uint8_t packet[128];
                size_t offset = 0;
                if (1 + sizeof(uuid) + uname_varint + uname_len + 1 > sizeof(packet)) {
                    printf("[LOGIN] ERROR: Login Success packet would overflow fixed buffer.\n");
                    free(username);
                    close_client_socket(new_socket);
                    return;
                }
                offset += write_varint(packet + offset, 0x02); // Login Success packet id
                memcpy(packet + offset, uuid, sizeof(uuid));
                offset += sizeof(uuid);
                memcpy(packet + offset, unamebuf, uname_varint + uname_len);
                offset += uname_varint + uname_len;
                // Add properties (VarInt 0 for empty array)
                offset += write_varint(packet + offset, 0);

                // Print raw Login Success packet (no length prefix)
                if (g_log_packet_framing) {
                    printf("Raw Login Success packet (hex): ");
                    for (size_t i = 0; i < offset; ++i) printf("%02X ", packet[i]);
                    printf("\n");
                }

                // Login Success double-framing (pass only raw packet, no length prefix)
                uint8_t double_framed[512];
                size_t double_framed_len = double_frame_packet(double_framed, sizeof(double_framed), packet, offset);
                if (g_log_packet_framing) {
                    printf("Double Framed Login Success packet (hex): ");
                    for (size_t i = 0; i < double_framed_len; ++i) printf("%02X ", double_framed[i]);
                    printf("\n");
                }
                // Send Login Success
#ifdef _WIN32
                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                send(new_socket, double_framed, double_framed_len, 0);
#endif
                printf("Sent Login Success to client.\n");

                // Wait for Login Acknowledged (0x03) from client
                {
                    uint8_t ack_buf[256];
#ifdef _WIN32
                    int ack_bytes = recv(new_socket, (char*)ack_buf, sizeof(ack_buf), 0);
#else
                    ssize_t ack_bytes = recv(new_socket, ack_buf, sizeof(ack_buf), 0);
#endif
                    if (ack_bytes > 0) {
                        if (g_log_packet_framing) {
                            printf("Received packet after Login Success: ");
                            for (int i = 0; i < ack_bytes; ++i) printf("%02X ", ack_buf[i]);
                            printf("\n");
                        }
                        {
                            int32_t ackpid = -1;
                            if (!read_post_compression_packet_id(ack_buf, (size_t)ack_bytes, &ackpid)) {
                                printf("Failed to parse packet after Login Success (post-compression format).\n");
                                free(username);
                                close_client_socket(new_socket);
                                return;
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
                                uint8_t *cfg_buf = NULL;
                                size_t cfg_buf_cap = 8192;
                                size_t cfg_len;
                                uint8_t *double_framed = NULL;
                                size_t double_framed_cap = 16384;
                                size_t double_framed_len;

                                cfg_buf = (uint8_t *)malloc(cfg_buf_cap);
                                double_framed = (uint8_t *)malloc(double_framed_cap);
                                if (cfg_buf == NULL || double_framed == NULL) {
                                    printf("[LOGIN] ERROR: Failed to allocate config staging buffers.\n");
                                    free(double_framed);
                                    free(cfg_buf);
                                    free(username);
                                    close_client_socket(new_socket);
                                    return;
                                }

                                // Known Packs (required for proper registry bootstrap in 1.21+)
                                cfg_len = build_known_packs_packet(cfg_buf, cfg_buf_cap);
                                double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                send(new_socket, double_framed, double_framed_len, 0);
#endif
                                printf("Sent Known Packs to client.\n");

                                // Wait for Serverbound Known Packs (id 0x07).
                                {
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

                                        {
                                            int32_t kp_pid = -1;
                                            if (!read_post_compression_packet_id(kp_buf, (size_t)kp_bytes, &kp_pid)) {
                                                printf("Failed to parse config packet during known-packs negotiation.\n");
                                                continue;
                                            }
                                            if (g_log_packet_framing) {
                                                printf("Parsed config packet ID: %d\n", kp_pid);
                                            }
                                            if (kp_pid == 0x07) {
                                                got_known_packs = 1;
                                                break;
                                            }
                                        }
                                    }

                                    if (!got_known_packs) {
                                        printf("Did not receive Serverbound Known Packs (0x07).\n");
                                        free(double_framed);
                                        free(cfg_buf);
                                        free(username);
                                        close_client_socket(new_socket);
                                        return;
                                    }
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
                                        double_framed_len = double_frame_packet(double_framed, double_framed_cap, replay.packets[i], replay.lengths[i]);
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
                                        cfg_len = build_finish_config_packet(cfg_buf, cfg_buf_cap);
                                        double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                        send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                        send(new_socket, double_framed, double_framed_len, 0);
#endif
                                        printf("Sent Finish Configuration to client.\n");
                                    }
                                } else {
                                    // Minimal fallback when no replay file is present.
                                    cfg_len = build_feature_flags_packet(cfg_buf, cfg_buf_cap);
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Feature Flags to client.\n");

                                    cfg_len = build_registry_data_packet(cfg_buf, cfg_buf_cap);
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (dimension_type) to client.\n");

                                    cfg_len = build_registry_data_biome_packet(cfg_buf, cfg_buf_cap);
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (biome) to client.\n");

                                    cfg_len = build_registry_data_damage_type(cfg_buf, cfg_buf_cap);
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (damage_type) to client.\n");

                                    // Required non-empty dynamic registries added in 1.21.5+
                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:cat_variant", "minecraft:tabby");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (cat_variant) to client.\n");

                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:chicken_variant", "minecraft:temperate");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (chicken_variant) to client.\n");

                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:cow_variant", "minecraft:temperate");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (cow_variant) to client.\n");

                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:frog_variant", "minecraft:temperate");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (frog_variant) to client.\n");

                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:painting_variant", "minecraft:kebab");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (painting_variant) to client.\n");

                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:pig_variant", "minecraft:temperate");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (pig_variant) to client.\n");

                                    cfg_len = build_registry_data_inline_empty(cfg_buf, cfg_buf_cap, "minecraft:timeline", "minecraft:overworld");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (timeline) to client.\n");

                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:wolf_sound_variant", "minecraft:classic");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (wolf_sound_variant) to client.\n");

                                    cfg_len = build_registry_data_one(cfg_buf, cfg_buf_cap, "minecraft:wolf_variant", "minecraft:pale");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (wolf_variant) to client.\n");

                                    cfg_len = build_registry_data_with_asset_id(cfg_buf, cfg_buf_cap, "minecraft:zombie_nautilus_variant", "minecraft:temperate", "minecraft:zombie_nautilus");
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Registry Data (zombie_nautilus_variant) to client.\n");

                                    // Update Tags: bind minecraft:timeline#minecraft:in_overworld to entry 0
                                    cfg_len = build_update_tags_with_timeline(cfg_buf, cfg_buf_cap);
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Update Tags to client.\n");

                                    cfg_len = build_finish_config_packet(cfg_buf, cfg_buf_cap);
                                    double_framed_len = double_frame_packet(double_framed, double_framed_cap, cfg_buf, cfg_len);
#ifdef _WIN32
                                    send(new_socket, (const char*)double_framed, (int)double_framed_len, 0);
#else
                                    send(new_socket, double_framed, double_framed_len, 0);
#endif
                                    printf("Sent Finish Configuration to client.\n");
                                }

                                // Wait for Acknowledge Finish Configuration (serverbound id 0x03).
                                // The client may send other configuration packets first (e.g. plugin/client info).
                                {
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

                                        {
                                            int32_t cfg_pid = -1;
                                            if (!read_post_compression_packet_id(cfg_ack_buf, (size_t)cfg_ack_bytes, &cfg_pid)) {
                                                printf("Failed to parse config packet (post-compression format).\n");
                                                continue;
                                            }

                                            if (g_log_packet_framing) {
                                                printf("Parsed config packet ID: %d\n", cfg_pid);
                                            }
                                            if (cfg_pid == 0x03) {
                                                got_finish_ack = 1;
                                                break;
                                            }
                                        }
                                    }

                                    if (!got_finish_ack) {
                                        free_config_replay(&replay);
                                        printf("Did not receive Acknowledge Finish Configuration (0x03).\n");
                                        free(double_framed);
                                        free(cfg_buf);
                                        free(username);
                                        close_client_socket(new_socket);
                                        return;
                                    }
                                }

                                free_config_replay(&replay);
                                free(double_framed);
                                free(cfg_buf);

                                // Load world metadata for play bootstrap if a world folder is available.
                                {
                                    world_info_t world_info;
                                    char world_error[256];
                                    int startup_force_debug_spawn = server_config.force_debug_spawn || env_flag_enabled("VECTORA_FORCE_DEBUG_SPAWN");
                                    int startup_world_source_mode = startup_force_debug_spawn ? WORLD_SOURCE_MODE_DEBUG : server_config.world_source_mode;
                                    int should_attempt_world_load =
                                        (startup_world_source_mode == WORLD_SOURCE_MODE_AUTO || startup_world_source_mode == WORLD_SOURCE_MODE_REAL);
                                    int has_world_info = 0;

                                    if (should_attempt_world_load) {
                                        has_world_info = load_world_info(
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
                                    } else {
                                        printf("Skipping world folder load because world source mode is %s.\n",
                                               world_source_mode_name(startup_world_source_mode));
                                    }

                                    {
                                        int force_debug_spawn = server_config.force_debug_spawn || env_flag_enabled("VECTORA_FORCE_DEBUG_SPAWN");
                                        int effective_world_source_mode = force_debug_spawn ? WORLD_SOURCE_MODE_DEBUG : server_config.world_source_mode;
                                        if (force_debug_spawn) {
                                            printf("VECTORA_FORCE_DEBUG_SPAWN enabled: forcing debug chunks and debug spawn.\n");
                                        }
                                        printf("World source mode resolved to %s (configured=%s, real_chunks=%s, debug_fallback=%s).\n",
                                               world_source_mode_name(effective_world_source_mode),
                                               world_source_mode_name(server_config.world_source_mode),
                                               server_config.enable_real_chunks ? "on" : "off",
                                               server_config.allow_debug_chunk_fallback ? "on" : "off");

                                        // Join Game
                                        {
                                            uint8_t jg_buf[1024];
                                            join_game_params_t join_params;
                                            memset(&join_params, 0, sizeof(join_params));
                                            join_params.entity_id = 1;
                                            join_params.dimension_name = has_world_info ? world_info.dimension_name : "minecraft:overworld";
                                            join_params.max_players = server_config.max_players;
                                            join_params.view_distance = server_config.view_distance;
                                            join_params.simulation_distance = server_config.simulation_distance;
                                            join_params.game_mode = (uint8_t)server_config.game_mode;
                                            join_params.is_hardcore = server_config.is_hardcore;
                                            join_params.previous_game_mode = -1;
                                            join_params.sea_level = has_world_info ? world_info.sea_level : 63;
                                            join_params.is_flat = has_world_info ? world_info.is_flat : 0;
                                            size_t jg_len = build_join_game_packet_ex(jg_buf, sizeof(jg_buf), &join_params);
                                            send_post_compression_packet(new_socket, jg_buf, jg_len);
                                            printf("Sent Join Game to client.\n");
                                        }

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

                                        // Game Event 11: enable respawn screen / immediate respawn.
                                        {
                                            uint8_t ge_buf[32];
                                            size_t ge_len = build_game_event_packet(
                                                ge_buf,
                                                sizeof(ge_buf),
                                                11,
                                                (float)server_config.game_event_respawn_screen_value);
                                            send_post_compression_packet(new_socket, ge_buf, ge_len);
                                            printf("Sent Game Event 11 (Respawn Screen=%d).\n",
                                                   server_config.game_event_respawn_screen_value);
                                        }

                                        // Game Event 12: limited crafting toggle.
                                        {
                                            uint8_t ge_buf[32];
                                            size_t ge_len = build_game_event_packet(
                                                ge_buf,
                                                sizeof(ge_buf),
                                                12,
                                                (float)server_config.game_event_limited_crafting_value);
                                            send_post_compression_packet(new_socket, ge_buf, ge_len);
                                            printf("Sent Game Event 12 (Limited Crafting=%d).\n",
                                                   server_config.game_event_limited_crafting_value);
                                        }

                                        {
                                            int32_t debug_chunk_x = has_world_info ? world_info.spawn_chunk_x : 0;
                                            int32_t debug_chunk_z = has_world_info ? world_info.spawn_chunk_z : 0;
                                            int32_t debug_spawn_x = debug_chunk_x * 16 + 8;
                                            int32_t debug_spawn_z = debug_chunk_z * 16 + 8;
                                            int32_t debug_spawn_y = generated_world_surface_y(debug_spawn_x, debug_spawn_z) + 2;
                                            int32_t player_spawn_x = debug_spawn_x;
                                            int32_t player_spawn_y = debug_spawn_y;
                                            int32_t player_spawn_z = debug_spawn_z;
                                            chunk_send_result_t center_chunk_result = CHUNK_SEND_RESULT_NONE;
                                            int use_real_spawn = 0;
                                            session->stream_state.chunk_stream_radius =
                                                resolve_chunk_stream_radius_for_world_source(&server_config,
                                                                                             effective_world_source_mode);
                                              printf("Chunk stream radius resolved to %d for world_source=%s.\n",
                                                  session->stream_state.chunk_stream_radius,
                                                  world_source_mode_name(effective_world_source_mode));
                                            session->stream_state.stream_center_chunk_x = debug_chunk_x;
                                            session->stream_state.stream_center_chunk_z = debug_chunk_z;
                                            session->stream_state.force_debug_spawn = force_debug_spawn;
                                            session->stream_state.has_world_info = has_world_info;

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

                                                {
                                                    int sent_chunks = 0;

                                                    for (int dz = -session->stream_state.chunk_stream_radius; dz <= session->stream_state.chunk_stream_radius; dz++) {
                                                        for (int dx = -session->stream_state.chunk_stream_radius; dx <= session->stream_state.chunk_stream_radius; dx++) {
                                                            int32_t sx = chunk_x + dx;
                                                            int32_t sz = chunk_z + dz;
                                                            chunk_send_result_t send_result = CHUNK_SEND_RESULT_NONE;

                                                            if (send_stream_chunk(session, sx, sz, &world_info, &send_result)) {
                                                                sent_chunks++;
                                                                if (sx == chunk_x && sz == chunk_z) {
                                                                    center_chunk_result = send_result;
                                                                }
                                                            } else {
                                                                printf("WARNING: no chunk sent for (%d,%d) with world_source=%s.\n",
                                                                       sx,
                                                                       sz,
                                                                       world_source_mode_name(effective_world_source_mode));
                                                            }
                                                        }
                                                    }

                                                    // ChunkBatchFinished (0x0B)
                                                    {
                                                        uint8_t cbf_buf[8];
                                                        size_t cbf_len = 0;
                                                        cbf_len += write_varint(cbf_buf + cbf_len, 0x0B);
                                                        cbf_len += write_varint(cbf_buf + cbf_len, sent_chunks);
                                                        send_post_compression_packet(new_socket, cbf_buf, cbf_len);
                                                        printf("Sent ChunkBatchFinished (batch size=%d).\n", sent_chunks);
                                                    }

                                                    use_real_spawn = (center_chunk_result == CHUNK_SEND_RESULT_REAL) && has_world_info;
                                                    if (use_real_spawn) {
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
                                                        if (center_chunk_result == CHUNK_SEND_RESULT_GENERATED) {
                                                            printf("Using generated-world spawn at (%d,%d,%d).\n", player_spawn_x, player_spawn_y, player_spawn_z);
                                                        } else {
                                                            printf("Using debug spawn at (%d,%d,%d).\n", player_spawn_x, player_spawn_y, player_spawn_z);
                                                        }
                                                    }
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

                                            // Game Rules are not currently validated for protocol 774.
                                            // Packet ID 0x5D collides with a different clientbound play packet there,
                                            // so do not send this packet on that protocol even when enabled in config.
                                            if (server_config.send_game_rules_packet &&
                                                session->client_protocol_version == server_config.protocol_number &&
                                                server_config.protocol_number != 774) {
                                                uint8_t game_rules_buf[4096];
                                                size_t game_rules_len = build_game_rules_packet(game_rules_buf, sizeof(game_rules_buf), &server_config.game_rules);
                                                send_post_compression_packet(new_socket, game_rules_buf, game_rules_len);
                                                printf("Sent Game Rules packet (%zu bytes).\n", game_rules_len);
                                            } else if (server_config.send_game_rules_packet) {
                                                if (session->client_protocol_version != server_config.protocol_number) {
                                                    printf("Skipped Game Rules packet due to protocol mismatch (client=%d, configured=%d).\n",
                                                           session->client_protocol_version,
                                                           server_config.protocol_number);
                                                } else if (server_config.protocol_number == 774) {
                                                    printf("Skipped Game Rules packet because it is not validated for protocol 774.\n");
                                                }
                                            }

                                            // Player Position and Look
                                            {
                                                uint8_t pos_buf[128];
                                                player_pos_params_t pos_params;
                                                memset(&pos_params, 0, sizeof(pos_params));
                                                pos_params.teleport_id = 1;
                                                pos_params.x = (double)player_spawn_x + 0.5;
                                                pos_params.y = (double)player_spawn_y;
                                                pos_params.z = (double)player_spawn_z + 0.5;
                                                pos_params.yaw = use_real_spawn ? world_info.spawn_yaw : 0.0f;
                                                pos_params.pitch = use_real_spawn ? world_info.spawn_pitch : 0.0f;
                                                {
                                                    size_t pos_len = build_player_pos_packet_ex(pos_buf, sizeof(pos_buf), &pos_params);
                                                    send_post_compression_packet(new_socket, pos_buf, pos_len);
                                                }
                                                printf("Sent Player Position and Look to client.\n");

                                                if (!entity_manager_queue_spawn(&session->entity_registry,
                                                                                &session->entity_manager,
                                                                                1,
                                                                                ENTITY_KIND_PLAYER,
                                                                                pos_params.x,
                                                                                pos_params.y,
                                                                                pos_params.z)) {
                                                    printf("WARNING: failed to register local player entity in registry.\n");
                                                }
                                            }

                                            if (server_config.enable_experimental_entities) {
                                                printf("Experimental entities enabled: packet sends are still guarded until per-packet validation is complete.\n");
                                                if (!server_config.enable_experimental_entity_packets) {
                                                    printf("Outbound entity packets are disabled by config gate (enable_experimental_entity_packets=false).\n");
                                                }

                                                {
                                                    int32_t mock_a = 0;
                                                    int32_t mock_b = 0;
                                                    int spawned_a = entity_manager_queue_spawn_auto(
                                                        &session->entity_registry,
                                                        &session->entity_manager,
                                                        ENTITY_KIND_MOB,
                                                        (double)player_spawn_x + 2.0,
                                                        (double)player_spawn_y,
                                                        (double)player_spawn_z + 1.0,
                                                        &mock_a);
                                                    int spawned_b = entity_manager_queue_spawn_auto(
                                                        &session->entity_registry,
                                                        &session->entity_manager,
                                                        ENTITY_KIND_MOB,
                                                        (double)player_spawn_x - 2.0,
                                                        (double)player_spawn_y,
                                                        (double)player_spawn_z - 1.0,
                                                        &mock_b);
                                                    if (spawned_a && spawned_b) {
                                                        session->mock_entity_a = mock_a;
                                                        session->mock_entity_b = mock_b;
                                                        printf("Queued internal mock entities: ids=%d,%d.\n", mock_a, mock_b);

                                                        if (server_config.enable_experimental_entity_packets &&
                                                            session->client_protocol_version == 774) {
                                                            // Send Spawn Entity (0x01) for both mock entities.
                                                            // Entity type 5 = armor_stand (protocol 774 registry ID).
                                                            uint8_t se_buf[128];
                                                            size_t se_len;

                                                            se_len = build_spawn_entity_packet(
                                                                se_buf, sizeof(se_buf),
                                                                mock_a,
                                                                5, // entity type: armor_stand (protocol 774 ID)
                                                                (double)player_spawn_x + 2.0,
                                                                (double)player_spawn_y,
                                                                (double)player_spawn_z + 1.0,
                                                                0, 0, 0, 0);
                                                            send_post_compression_packet(new_socket, se_buf, se_len);

                                                            se_len = build_spawn_entity_packet(
                                                                se_buf, sizeof(se_buf),
                                                                mock_b,
                                                                5, // entity type: armor_stand (protocol 774 ID)
                                                                (double)player_spawn_x - 2.0,
                                                                (double)player_spawn_y,
                                                                (double)player_spawn_z - 1.0,
                                                                0, 0, 0, 0);
                                                            send_post_compression_packet(new_socket, se_buf, se_len);

                                                            {
                                                                uint8_t md_buf[32];
                                                                size_t md_len;
                                                                md_len = build_set_entity_glowing_packet(md_buf, sizeof(md_buf), mock_a, 1);
                                                                send_post_compression_packet(new_socket, md_buf, md_len);
                                                                md_len = build_set_entity_glowing_packet(md_buf, sizeof(md_buf), mock_b, 1);
                                                                send_post_compression_packet(new_socket, md_buf, md_len);
                                                            }

                                                            session->mock_entity_a_visible = 1;
                                                            session->mock_entity_b_visible = 1;

                                                            printf("Sent Spawn Entity for mock entities %d and %d.\n", mock_a, mock_b);
                                                        }
                                                    }

                                                    if (server_config.enable_experimental_entity_packets &&
                                                        session->client_protocol_version == 774) {
                                                        static const int32_t starter_mob_types[] = {
                                                            30, 100, 111, 150, 115, 32, 124, 41
                                                        };
                                                        size_t type_count = sizeof(starter_mob_types) / sizeof(starter_mob_types[0]);
                                                        size_t mob_count = (size_t)server_config.entity_starter_spawn_count;
                                                        size_t spawned_mobs = 0;

                                                        if (mob_count > 32) {
                                                            mob_count = 32;
                                                        }

                                                        for (size_t mi = 0; mi < mob_count; ++mi) {
                                                            int32_t mob_id = 0;
                                                            int tracked_idx = -1;
                                                            double angle = ((double)mi / (double)mob_count) * 6.28318530718;
                                                            double sx = (double)player_spawn_x + cos(angle) * 6.0;
                                                            double sz = (double)player_spawn_z + sin(angle) * 6.0;
                                                            double sy = resolve_generated_ground_y(sx, sz);

                                                            if (!entity_manager_queue_spawn_auto(&session->entity_registry,
                                                                                                 &session->entity_manager,
                                                                                                 ENTITY_KIND_MOB,
                                                                                                 sx,
                                                                                                 sy,
                                                                                                 sz,
                                                                                                 &mob_id)) {
                                                                continue;
                                                            }

                                                            if (track_spawned_entity_id(session, mob_id, starter_mob_types[mi % type_count])) {
                                                                tracked_idx = find_tracked_spawned_entity_index(session, mob_id);
                                                            }

                                                            if (tracked_idx >= 0) {
                                                                session->spawned_entity_visible[tracked_idx] = 0;
                                                                reconcile_entity_visibility(session,
                                                                                            new_socket,
                                                                                            mob_id,
                                                                                            starter_mob_types[mi % type_count],
                                                                                            0,
                                                                                            &session->spawned_entity_visible[tracked_idx]);
                                                                spawned_mobs += 1;
                                                            }
                                                        }

                                                        if (spawned_mobs > 0) {
                                                            printf("Spawned %zu roaming starter mobs for free-movement testing.\n", spawned_mobs);
                                                        }
                                                    }
                                                }
                                            }

                                            run_play_state_loop(
                                                session,
                                                new_socket,
                                                server_fd,
                                                has_world_info,
                                                force_debug_spawn,
                                                &world_info);

                                            if (has_world_info) {
                                                free_world_info(&world_info);
                                            }
                                        }
                                    }

                                    close_client_socket(new_socket);
                                }
                            } else {
                                printf("Did not receive Login Acknowledged (got id=%d)\n", ackpid);
                                free(username);
                                close_client_socket(new_socket);
                                return;
                            }
                        }
                    } else {
                        printf("No packet received after Login Success.\n");
                        free(username);
                        close_client_socket(new_socket);
                        return;
                    }
                }

                free(username);
            }
        }
}

#ifdef _WIN32
static DWORD WINAPI client_worker_thread(LPVOID arg) {
    client_session_t *session = (client_session_t*)arg;
    if (session) {
        append_lifecycle_log("[THREAD] start socket=%llu", (unsigned long long)session->socket_fd);
        __try {
            handle_client_connection(session);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            DWORD code = GetExceptionCode();
            fprintf(stderr,
                    "[FATAL] Unhandled exception in client worker thread (code=0x%08lX).\n",
                    (unsigned long)code);
            fflush(stderr);
            if (session->socket_fd != INVALID_SOCKET) {
                close_client_socket(session->socket_fd);
            }
        }
        append_lifecycle_log("[THREAD] end socket=%llu", (unsigned long long)session->socket_fd);
        free(session);
    }
    decrement_active_connections();
    return 0;
}
#else
static void *client_worker_thread(void *arg) {
    client_session_t *session = (client_session_t*)arg;
    if (session) {
        handle_client_connection(session);
        free(session);
    }
    decrement_active_connections();
    return NULL;
}
#endif

int main() {
    server_config_t server_config;
    char server_config_error[256];
    const char *server_config_path = NULL;
    int server_config_status;

#ifdef _WIN32
    WSADATA wsaData;

    SetUnhandledExceptionFilter(vectora_unhandled_exception_filter);
    _set_invalid_parameter_handler(vectora_invalid_parameter_handler);
    signal(SIGABRT, vectora_signal_handler);
    signal(SIGSEGV, vectora_signal_handler);
    signal(SIGILL, vectora_signal_handler);
    signal(SIGFPE, vectora_signal_handler);
    signal(SIGTERM, vectora_signal_handler);
    atexit(vectora_process_exit_marker);

    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
    }
#endif
    
    /* Initialize light data early to avoid thread-safety issues */
    extern void init_light_data_once(void);
    init_light_data_once();
    
    init_play_chat_lock();
    load_moderation_state();

    server_config_status = load_server_config_with_fallbacks(
        &server_config,
        &server_config_path,
        server_config_error,
        sizeof(server_config_error));
    if (server_config_status < 0) {
        fprintf(stderr, "Failed to load server config: %s\n", server_config_error);
        destroy_play_chat_lock();
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

    {
        int startup_force_debug_spawn = server_config.force_debug_spawn || env_flag_enabled("VECTORA_FORCE_DEBUG_SPAWN");
        int startup_world_source_mode = startup_force_debug_spawn ? WORLD_SOURCE_MODE_DEBUG : server_config.world_source_mode;

        printf("World source mode: %s (configured=%s, real_chunks=%s, debug_fallback=%s).\n",
               world_source_mode_name(startup_world_source_mode),
               world_source_mode_name(server_config.world_source_mode),
               server_config.enable_real_chunks ? "on" : "off",
               server_config.allow_debug_chunk_fallback ? "on" : "off");
    }

    ensure_generated_world_scaffold(&server_config);

    {
        int startup_force_debug_spawn = server_config.force_debug_spawn || env_flag_enabled("VECTORA_FORCE_DEBUG_SPAWN");
        int startup_world_source_mode = startup_force_debug_spawn ? WORLD_SOURCE_MODE_DEBUG : server_config.world_source_mode;
        int prewarm_radius = resolve_chunk_stream_radius_for_world_source(&server_config,
                                                                           startup_world_source_mode);
        if (startup_world_source_mode == WORLD_SOURCE_MODE_GENERATED) {
            if (prewarm_radius > 2) {
                prewarm_radius = 2;
            }
            prewarm_generated_chunk_cache(&server_config, 0, 0, prewarm_radius);
            if (server_config.log_generated_region_summary) {
                log_generated_region_index_summary(&server_config, 0, 0, prewarm_radius);
            }
        }
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

    g_listen_socket = server_fd;
    append_lifecycle_log("[MAIN] listen socket created handle=%llu", (unsigned long long)server_fd);

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
    append_lifecycle_log("[MAIN] listening on port %d", server_config.port);

    while (1) {
        addrlen = sizeof(address);
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
    #ifdef _WIN32
        if (new_socket == INVALID_SOCKET) {
    #else
        if (new_socket < 0) {
    #endif
            perror("accept");
    #ifdef _WIN32
            append_lifecycle_log("[MAIN] accept failed WSA=%d", WSAGetLastError());
    #else
            append_lifecycle_log("[MAIN] accept failed errno=%d", errno);
    #endif
            continue;
        }

        append_lifecycle_log("[MAIN] accepted socket=%llu", (unsigned long long)new_socket);

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

        printf("New connection accepted!\n");

        // Check if we've reached max connections
        if (load_active_connections() >= server_config.max_connections) {
            fprintf(stderr, "Max connections (%d) reached, rejecting new connection.\n", server_config.max_connections);
            close_client_socket(new_socket);
            continue;
        }

        {
            client_session_t *session = (client_session_t*)malloc(sizeof(client_session_t));
            if (!session) {
                fprintf(stderr, "Failed to allocate client session.\n");
                close_client_socket(new_socket);
                continue;
            }

            memset(session, 0, sizeof(*session));
            session->socket_fd = new_socket;
            session->server_fd = server_fd;
            session->server_config = server_config;
            snprintf(session->username, sizeof(session->username), "Player");
            entity_registry_init(&session->entity_registry);
            entity_manager_init(&session->entity_manager);

            // Increment before spawning thread to avoid race condition
            increment_active_connections();

#ifdef _WIN32
            {
                SIZE_T worker_stack_size = 8 * 1024 * 1024;
                HANDLE thread_handle = CreateThread(NULL,
                                                    worker_stack_size,
                                                    client_worker_thread,
                                                    session,
                                                    0,
                                                    NULL);
                if (thread_handle == NULL) {
                    fprintf(stderr, "CreateThread failed for client session.\n");
                    close_client_socket(new_socket);
                    free(session);
                    decrement_active_connections();
                    continue;
                }
                CloseHandle(thread_handle);
            }
#else
            {
                pthread_t thread;
                if (pthread_create(&thread, NULL, client_worker_thread, session) != 0) {
                    fprintf(stderr, "pthread_create failed for client session.\n");
                    close_client_socket(new_socket);
                    free(session);
                    decrement_active_connections();
                    continue;
                }
                pthread_detach(thread);
            }
#endif
        }
    }

#ifdef _WIN32
    closesocket(server_fd);
    g_listen_socket = INVALID_SOCKET;
    (void)save_moderation_state();
    destroy_play_chat_lock();
    WSACleanup();
#else
    g_listen_socket = -1;
    (void)save_moderation_state();
    destroy_play_chat_lock();
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

