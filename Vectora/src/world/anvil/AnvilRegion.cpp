#include "world/AnvilRegion.h"
#include <fstream>
#include <iostream>
#include <cstring>

AnvilRegion::AnvilRegion(const std::string& regionPath) : path(regionPath), valid(false) {
    parseRegionHeader();
}

bool AnvilRegion::isValid() const {
    return valid;
}

std::string AnvilRegion::getRegionFileName(int regionX, int regionZ) {
    char buf[64];
    snprintf(buf, sizeof(buf), "r.%d.%d.mca", regionX, regionZ);
    return std::string(buf);
}

void AnvilRegion::parseRegionHeader() {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "[Anvil] Failed to open region file: " << path << std::endl;
        valid = false;
        return;
    }
    chunkOffsets.resize(1024);
    chunkSizes.resize(1024);
    file.read(reinterpret_cast<char*>(chunkOffsets.data()), 4096);
    file.seekg(4096, std::ios::beg);
    file.read(reinterpret_cast<char*>(chunkSizes.data()), 4096);
    valid = true;
}

std::shared_ptr<ChunkData> AnvilRegion::loadChunk(int chunkX, int chunkZ) {
    if (!valid) return nullptr;
    int idx = (chunkX & 31) + (chunkZ & 31) * 32;
    uint32_t offset = (chunkOffsets[idx] >> 8) * 4096;
    uint32_t size = (chunkOffsets[idx] & 0xFF) * 4096;
    if (offset == 0) return nullptr;
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    file.seekg(offset, std::ios::beg);
    uint32_t length = 0;
    file.read(reinterpret_cast<char*>(&length), 4);
    uint8_t compressionType = 0;
    file.read(reinterpret_cast<char*>(&compressionType), 1);
    std::vector<uint8_t> rawData(length - 1);
    file.read(reinterpret_cast<char*>(rawData.data()), length - 1);
    auto chunk = std::make_shared<ChunkData>();
    chunk->x = chunkX;
    chunk->z = chunkZ;
    chunk->rawData = std::move(rawData);
    // TODO: Decompress and parse NBT
    return chunk;
}
