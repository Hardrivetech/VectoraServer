#include "world/ZlibUtil.h"
#include <zlib.h>
#include <iostream>

bool decompressZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    if (input.empty()) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)input.data();
    strm.avail_in = input.size();
    if (inflateInit(&strm) != Z_OK) return false;
    output.resize(1024 * 1024); // 1MB buffer for decompressed chunk
    strm.next_out = output.data();
    strm.avail_out = output.size();
    int ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END && ret != Z_OK) {
        inflateEnd(&strm);
        std::cerr << "[Zlib] Inflate failed: " << ret << std::endl;
        return false;
    }
    output.resize(strm.total_out);
    inflateEnd(&strm);
    return true;
}
