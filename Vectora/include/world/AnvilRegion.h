#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

struct Chunk;
struct ChunkData {
    int x, z;
    uint8_t compressionType = 0;
    std::vector<uint8_t> rawData;
    std::shared_ptr<Chunk> parsedChunk; // Parsed block/entity data
};

class AnvilRegion {
public:
    AnvilRegion(const std::string& regionPath);
    bool isValid() const;
    std::shared_ptr<ChunkData> loadChunk(int chunkX, int chunkZ);
    static std::string getRegionFileName(int regionX, int regionZ);
private:
    std::string path;
    bool valid;
    std::vector<uint32_t> chunkOffsets;
    std::vector<uint32_t> chunkSizes;
    void parseRegionHeader();
};
