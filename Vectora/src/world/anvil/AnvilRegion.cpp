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
    // Print first 64 bytes of the region file in hex for debugging
    file.seekg(0, std::ios::beg);
    uint8_t first64[64] = {0};
    file.read(reinterpret_cast<char*>(first64), 64);
    std::cout << "[Anvil] First 64 bytes of region file:";
    for (int i = 0; i < 64; ++i) {
        if (i % 16 == 0) std::cout << "\n[Anvil] ";
        std::cout << std::hex << (int)first64[i] << " ";
    }
    std::cout << std::dec << std::endl;
    file.clear();
    file.seekg(0, std::ios::beg);
    chunkOffsets.resize(1024);
    chunkSizes.resize(1024);
    // Read chunk offset table as big-endian 4-byte values
    uint8_t offsetTable[4096];
    file.read(reinterpret_cast<char*>(offsetTable), 4096);
    for (int i = 0; i < 1024; ++i) {
        chunkOffsets[i] = (offsetTable[i * 4 + 0] << 24) |
                          (offsetTable[i * 4 + 1] << 16) |
                          (offsetTable[i * 4 + 2] << 8)  |
                          (offsetTable[i * 4 + 3]);
    }
    file.seekg(4096, std::ios::beg);
    file.read(reinterpret_cast<char*>(chunkSizes.data()), 4096);
    valid = true;
}

std::shared_ptr<ChunkData> AnvilRegion::loadChunk(int chunkX, int chunkZ) {
    if (!valid) return nullptr;
    int idx = (chunkX & 31) + (chunkZ & 31) * 32;
    std::cout << "[Anvil] Debug: chunkX=" << chunkX << ", chunkZ=" << chunkZ << ", idx=" << idx << std::endl;
    std::cout << "[Anvil] Debug: chunkOffsets[" << idx << "] = 0x" << std::hex << chunkOffsets[idx] << std::dec << std::endl;
    for (int i = 0; i < 10; ++i) {
        std::cout << "[Anvil] chunkOffsets[" << i << "] = 0x" << std::hex << chunkOffsets[i] << std::dec << std::endl;
    }
    uint32_t sectorOffset = (chunkOffsets[idx] >> 8) & 0xFFFFFF; // Only lower 3 bytes
    uint8_t sectorCount = chunkOffsets[idx] & 0xFF; // Last byte
    uint32_t offset = sectorOffset * 4096;
    uint32_t size = sectorCount * 4096;
    if (offset == 0) return nullptr;
    std::ifstream file(path, std::ios::binary);
    if (!file) return nullptr;
    file.seekg(offset, std::ios::beg);
    uint8_t lengthBytes[4] = {0};
    file.read(reinterpret_cast<char*>(lengthBytes), 4);
    uint32_t length = (lengthBytes[0] << 24) | (lengthBytes[1] << 16) | (lengthBytes[2] << 8) | lengthBytes[3];
    uint8_t compressionType = 0;
    file.read(reinterpret_cast<char*>(&compressionType), 1);
    std::vector<uint8_t> rawData(length > 0 ? length - 1 : 0);
    if (length > 0) file.read(reinterpret_cast<char*>(rawData.data()), length - 1);
    std::cout << "[Anvil] Chunk at (" << chunkX << "," << chunkZ << ") offset: " << offset
              << ", size: " << size << ", length: " << length << ", compression: " << (int)compressionType << std::endl;
    if (compressionType != 2) {
        std::cerr << "[Anvil] Unsupported compression type for chunk at (" << chunkX << "," << chunkZ << "): " << (int)compressionType << std::endl;
    }
    auto chunk = std::make_shared<ChunkData>();
    chunk->x = chunkX;
    chunk->z = chunkZ;
    chunk->compressionType = compressionType;
    chunk->rawData = std::move(rawData);
    // TODO: Decompress and parse NBT
    return chunk;
}
