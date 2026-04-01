#include "world_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
} nbt_reader_t;

static void set_error(char *error, size_t error_size, const char *message) {
    if (error == NULL || error_size == 0) {
        return;
    }
    snprintf(error, error_size, "%s", message);
}

static int32_t floor_div32(int32_t value, int32_t divisor) {
    int32_t q = value / divisor;
    int32_t r = value % divisor;

    if (r != 0 && ((r < 0) != (divisor < 0))) {
        q -= 1;
    }
    return q;
}

static int read_file_all(const char *path, uint8_t **out_data, size_t *out_len) {
    FILE *fp;
    long file_len;
    uint8_t *data;

    *out_data = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    file_len = ftell(fp);
    if (file_len < 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    data = (uint8_t *)malloc((size_t)file_len);
    if (data == NULL) {
        fclose(fp);
        return 0;
    }

    if ((size_t)file_len != fread(data, 1, (size_t)file_len, fp)) {
        free(data);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    *out_data = data;
    *out_len = (size_t)file_len;
    return 1;
}

static int inflate_buffer_auto(const uint8_t *input, size_t input_len, uint8_t **out_data, size_t *out_len) {
    z_stream stream;
    size_t capacity = input_len * 4;
    uint8_t *buffer;
    int ret;

    if (capacity < 65536) {
        capacity = 65536;
    }

    buffer = (uint8_t *)malloc(capacity);
    if (buffer == NULL) {
        return 0;
    }

    memset(&stream, 0, sizeof(stream));
    stream.next_in = (Bytef *)input;
    stream.avail_in = (uInt)input_len;

    if (inflateInit2(&stream, 15 + 32) != Z_OK) {
        free(buffer);
        return 0;
    }

    for (;;) {
        if (stream.total_out == capacity) {
            uint8_t *grown;
            capacity *= 2;
            grown = (uint8_t *)realloc(buffer, capacity);
            if (grown == NULL) {
                inflateEnd(&stream);
                free(buffer);
                return 0;
            }
            buffer = grown;
        }

        stream.next_out = buffer + stream.total_out;
        stream.avail_out = (uInt)(capacity - stream.total_out);
        ret = inflate(&stream, Z_FINISH);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&stream);
            free(buffer);
            return 0;
        }
        if (ret == Z_BUF_ERROR && stream.avail_out != 0) {
            inflateEnd(&stream);
            free(buffer);
            return 0;
        }
    }

    *out_len = stream.total_out;
    inflateEnd(&stream);
    *out_data = buffer;
    return 1;
}

static int nbt_can_read(const nbt_reader_t *reader, size_t size) {
    return reader->pos + size <= reader->len;
}

static int nbt_read_u8(nbt_reader_t *reader, uint8_t *out) {
    if (!nbt_can_read(reader, 1)) {
        return 0;
    }
    *out = reader->data[reader->pos++];
    return 1;
}

static int nbt_read_be16(nbt_reader_t *reader, uint16_t *out) {
    if (!nbt_can_read(reader, 2)) {
        return 0;
    }
    *out = (uint16_t)((reader->data[reader->pos] << 8) | reader->data[reader->pos + 1]);
    reader->pos += 2;
    return 1;
}

static int nbt_read_be32(nbt_reader_t *reader, uint32_t *out) {
    if (!nbt_can_read(reader, 4)) {
        return 0;
    }
    *out = ((uint32_t)reader->data[reader->pos] << 24) |
           ((uint32_t)reader->data[reader->pos + 1] << 16) |
           ((uint32_t)reader->data[reader->pos + 2] << 8) |
           (uint32_t)reader->data[reader->pos + 3];
    reader->pos += 4;
    return 1;
}

static int nbt_read_name(nbt_reader_t *reader, const uint8_t **out_name, uint16_t *out_len) {
    if (!nbt_read_be16(reader, out_len)) {
        return 0;
    }
    if (!nbt_can_read(reader, *out_len)) {
        return 0;
    }
    *out_name = reader->data + reader->pos;
    reader->pos += *out_len;
    return 1;
}

static int nbt_name_equals(const uint8_t *name, uint16_t name_len, const char *literal) {
    size_t literal_len = strlen(literal);
    return name_len == literal_len && memcmp(name, literal, literal_len) == 0;
}

static int nbt_skip_payload(nbt_reader_t *reader, uint8_t tag_type);

static int nbt_skip_list(nbt_reader_t *reader) {
    uint8_t element_type;
    uint32_t count;
    uint32_t i;

    if (!nbt_read_u8(reader, &element_type) || !nbt_read_be32(reader, &count)) {
        return 0;
    }

    for (i = 0; i < count; ++i) {
        if (!nbt_skip_payload(reader, element_type)) {
            return 0;
        }
    }
    return 1;
}

static int nbt_skip_compound(nbt_reader_t *reader) {
    for (;;) {
        uint8_t tag_type;
        const uint8_t *name;
        uint16_t name_len;

        if (!nbt_read_u8(reader, &tag_type)) {
            return 0;
        }
        if (tag_type == 0) {
            return 1;
        }
        if (!nbt_read_name(reader, &name, &name_len)) {
            return 0;
        }
        if (!nbt_skip_payload(reader, tag_type)) {
            return 0;
        }
    }
}

static int nbt_skip_payload(nbt_reader_t *reader, uint8_t tag_type) {
    uint16_t string_len;
    uint32_t count;

    switch (tag_type) {
    case 1:
        reader->pos += 1;
        return reader->pos <= reader->len;
    case 2:
        reader->pos += 2;
        return reader->pos <= reader->len;
    case 3:
    case 5:
        reader->pos += 4;
        return reader->pos <= reader->len;
    case 4:
    case 6:
        reader->pos += 8;
        return reader->pos <= reader->len;
    case 7:
    case 11:
        if (!nbt_read_be32(reader, &count)) {
            return 0;
        }
        reader->pos += (tag_type == 7) ? count : count * 4u;
        return reader->pos <= reader->len;
    case 8:
        if (!nbt_read_be16(reader, &string_len)) {
            return 0;
        }
        reader->pos += string_len;
        return reader->pos <= reader->len;
    case 9:
        return nbt_skip_list(reader);
    case 10:
        return nbt_skip_compound(reader);
    case 12:
        if (!nbt_read_be32(reader, &count)) {
            return 0;
        }
        reader->pos += count * 8u;
        return reader->pos <= reader->len;
    default:
        return 0;
    }
}

static int nbt_read_int(nbt_reader_t *reader, int32_t *out) {
    uint32_t value;
    if (!nbt_read_be32(reader, &value)) {
        return 0;
    }
    *out = (int32_t)value;
    return 1;
}

static int nbt_read_float(nbt_reader_t *reader, float *out) {
    uint32_t bits;
    if (!nbt_read_be32(reader, &bits)) {
        return 0;
    }
    memcpy(out, &bits, sizeof(bits));
    return 1;
}

static int parse_level_data_compound(nbt_reader_t *reader, world_info_t *info) {
    for (;;) {
        uint8_t tag_type;
        const uint8_t *name;
        uint16_t name_len;

        if (!nbt_read_u8(reader, &tag_type)) {
            return 0;
        }
        if (tag_type == 0) {
            return 1;
        }
        if (!nbt_read_name(reader, &name, &name_len)) {
            return 0;
        }

        if (tag_type == 3 && nbt_name_equals(name, name_len, "SpawnX")) {
            if (!nbt_read_int(reader, &info->spawn_x)) {
                return 0;
            }
            continue;
        }
        if (tag_type == 3 && nbt_name_equals(name, name_len, "SpawnY")) {
            if (!nbt_read_int(reader, &info->spawn_y)) {
                return 0;
            }
            continue;
        }
        if (tag_type == 3 && nbt_name_equals(name, name_len, "SpawnZ")) {
            if (!nbt_read_int(reader, &info->spawn_z)) {
                return 0;
            }
            continue;
        }
        if (tag_type == 5 && nbt_name_equals(name, name_len, "SpawnAngle")) {
            if (!nbt_read_float(reader, &info->spawn_yaw)) {
                return 0;
            }
            continue;
        }

        if (!nbt_skip_payload(reader, tag_type)) {
            return 0;
        }
    }
}

static int parse_level_dat(const uint8_t *data, size_t data_len, world_info_t *info) {
    nbt_reader_t reader;
    uint8_t root_type;
    const uint8_t *root_name;
    uint16_t root_name_len;

    reader.data = data;
    reader.len = data_len;
    reader.pos = 0;

    if (!nbt_read_u8(&reader, &root_type) || root_type != 10) {
        return 0;
    }
    if (!nbt_read_name(&reader, &root_name, &root_name_len)) {
        return 0;
    }

    for (;;) {
        uint8_t tag_type;
        const uint8_t *name;
        uint16_t name_len;

        if (!nbt_read_u8(&reader, &tag_type)) {
            return 0;
        }
        if (tag_type == 0) {
            break;
        }
        if (!nbt_read_name(&reader, &name, &name_len)) {
            return 0;
        }
        if (tag_type == 10 && nbt_name_equals(name, name_len, "Data")) {
            if (!parse_level_data_compound(&reader, info)) {
                return 0;
            }
            continue;
        }
        if (!nbt_skip_payload(&reader, tag_type)) {
            return 0;
        }
    }

    return 1;
}

static int world_path_exists(const char *world_path) {
    char level_path[1024];
    FILE *fp;

    snprintf(level_path, sizeof(level_path), "%s/level.dat", world_path);
    fp = fopen(level_path, "rb");
    if (fp == NULL) {
        return 0;
    }
    fclose(fp);
    return 1;
}

static void path_dirname(char *path) {
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

static int get_executable_dir(char *buffer, size_t buffer_size) {
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, buffer, (DWORD)buffer_size);
    if (length == 0 || length >= buffer_size) {
        return 0;
    }
    path_dirname(buffer);
    return buffer[0] != '\0';
#else
    ssize_t length = readlink("/proc/self/exe", buffer, buffer_size - 1);
    if (length <= 0 || (size_t)length >= buffer_size) {
        return 0;
    }
    buffer[length] = '\0';
    path_dirname(buffer);
    return buffer[0] != '\0';
#endif
}

static int try_world_candidate(world_info_t *info, const char *candidate) {
    if (world_path_exists(candidate)) {
        snprintf(info->world_path, sizeof(info->world_path), "%s", candidate);
        return 1;
    }
    return 0;
}

static int resolve_world_path(world_info_t *info) {
    static const char *fallback_paths[] = {
        "world",
        "../world"
    };
    const char *env_path = getenv("VECTORA_WORLD_PATH");
    char exe_dir[1024];
    char candidate[1024];
    size_t i;

    if (env_path != NULL && env_path[0] != '\0' && try_world_candidate(info, env_path)) {
        return 1;
    }

    if (get_executable_dir(exe_dir, sizeof(exe_dir))) {
        snprintf(candidate, sizeof(candidate), "%s/world", exe_dir);
        if (try_world_candidate(info, candidate)) {
            return 1;
        }

        snprintf(candidate, sizeof(candidate), "%s/../world", exe_dir);
        if (try_world_candidate(info, candidate)) {
            return 1;
        }
    }

    for (i = 0; i < sizeof(fallback_paths) / sizeof(fallback_paths[0]); ++i) {
        if (try_world_candidate(info, fallback_paths[i])) {
            return 1;
        }
    }

    return 0;
}

static uint32_t read_be32_ptr(const uint8_t *src) {
    return ((uint32_t)src[0] << 24) |
           ((uint32_t)src[1] << 16) |
           ((uint32_t)src[2] << 8) |
           (uint32_t)src[3];
}

static int load_spawn_chunk(world_info_t *info) {
    char region_path[1024];
    uint8_t *region_file = NULL;
    size_t region_len = 0;
    int32_t region_x;
    int32_t region_z;
    int32_t local_x;
    int32_t local_z;
    size_t location_index;
    uint32_t location;
    uint32_t sector_offset;
    uint32_t sector_count;

    region_x = floor_div32(info->spawn_chunk_x, 32);
    region_z = floor_div32(info->spawn_chunk_z, 32);
    local_x = info->spawn_chunk_x - region_x * 32;
    local_z = info->spawn_chunk_z - region_z * 32;

    snprintf(region_path, sizeof(region_path), "%s/region/r.%d.%d.mca", info->world_path, region_x, region_z);
    if (!read_file_all(region_path, &region_file, &region_len)) {
        return 0;
    }
    if (region_len < 8192) {
        free(region_file);
        return 0;
    }

    location_index = (size_t)(local_x + local_z * 32) * 4;
    location = read_be32_ptr(region_file + location_index);
    sector_offset = location >> 8;
    sector_count = location & 0xFFu;
    if (sector_offset == 0 || sector_count == 0) {
        free(region_file);
        return 0;
    }

    {
        size_t chunk_offset = (size_t)sector_offset * 4096u;
        uint32_t chunk_length;
        uint8_t compression_type;
        uint8_t *chunk_payload;
        size_t chunk_payload_len;

        if (chunk_offset + 5 > region_len) {
            free(region_file);
            return 0;
        }

        chunk_length = read_be32_ptr(region_file + chunk_offset);
        if (chunk_length < 1 || chunk_offset + 4u + chunk_length > region_len) {
            free(region_file);
            return 0;
        }

        compression_type = region_file[chunk_offset + 4];
        chunk_payload = region_file + chunk_offset + 5;
        chunk_payload_len = (size_t)chunk_length - 1u;

        if (compression_type == 1 || compression_type == 2) {
            if (!inflate_buffer_auto(chunk_payload, chunk_payload_len, &info->spawn_chunk_nbt, &info->spawn_chunk_nbt_len)) {
                free(region_file);
                return 0;
            }
        } else if (compression_type == 3) {
            info->spawn_chunk_nbt = (uint8_t *)malloc(chunk_payload_len);
            if (info->spawn_chunk_nbt == NULL) {
                free(region_file);
                return 0;
            }
            memcpy(info->spawn_chunk_nbt, chunk_payload, chunk_payload_len);
            info->spawn_chunk_nbt_len = chunk_payload_len;
        } else {
            free(region_file);
            return 0;
        }
    }

    free(region_file);
    info->has_spawn_chunk = 1;
    return 1;
}

int load_world_info(world_info_t *info, char *error, size_t error_size) {
    char level_path[1024];
    uint8_t *compressed_level = NULL;
    size_t compressed_level_len = 0;
    uint8_t *level_nbt = NULL;
    size_t level_nbt_len = 0;

    if (info == NULL) {
        set_error(error, error_size, "invalid world info target");
        return 0;
    }

    memset(info, 0, sizeof(*info));
    snprintf(info->dimension_name, sizeof(info->dimension_name), "%s", "minecraft:overworld");
    info->spawn_y = 64;
    info->sea_level = 63;

    if (!resolve_world_path(info)) {
        set_error(error, error_size, "could not find a world folder");
        return 0;
    }

    snprintf(level_path, sizeof(level_path), "%s/level.dat", info->world_path);
    if (!read_file_all(level_path, &compressed_level, &compressed_level_len)) {
        set_error(error, error_size, "could not read level.dat");
        return 0;
    }
    if (!inflate_buffer_auto(compressed_level, compressed_level_len, &level_nbt, &level_nbt_len)) {
        free(compressed_level);
        set_error(error, error_size, "could not decompress level.dat");
        return 0;
    }
    free(compressed_level);

    if (!parse_level_dat(level_nbt, level_nbt_len, info)) {
        free(level_nbt);
        set_error(error, error_size, "could not parse level.dat NBT");
        return 0;
    }
    free(level_nbt);

    info->spawn_chunk_x = floor_div32(info->spawn_x, 16);
    info->spawn_chunk_z = floor_div32(info->spawn_z, 16);

    if (!load_spawn_chunk(info)) {
        info->has_spawn_chunk = 0;
    }

    return 1;
}

void free_world_info(world_info_t *info) {
    if (info == NULL) {
        return;
    }
    free(info->spawn_chunk_nbt);
    info->spawn_chunk_nbt = NULL;
    info->spawn_chunk_nbt_len = 0;
    info->has_spawn_chunk = 0;
}