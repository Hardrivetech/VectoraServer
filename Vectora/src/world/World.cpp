#include "world/World.h"
#include <iostream>


#include "world/AnvilRegion.h"
#include <memory>
#include <sstream>

// For demonstration, maintain a simple list of loaded chunks
#include <vector>
static std::vector<std::shared_ptr<ChunkData>> loadedChunks;

void World::tick() {
    std::cout << "[World] Ticking world..." << std::endl;
    // Example: iterate over loaded chunks and print their coordinates
    for (const auto& chunk : loadedChunks) {
        if (chunk) {
            std::cout << "[World] Chunk at x=" << chunk->x << ", z=" << chunk->z << std::endl;
        }
    }
    // Future: update entities, process block updates, etc.
}

std::shared_ptr<ChunkData> World::loadChunk(int chunkX, int chunkZ) {
    // Find region file for chunk
    int regionX = chunkX >> 5;
    int regionZ = chunkZ >> 5;
    std::ostringstream oss;
    oss << "world/region/" << AnvilRegion::getRegionFileName(regionX, regionZ);
    AnvilRegion region(oss.str());
    if (!region.isValid()) return nullptr;
    return region.loadChunk(chunkX, chunkZ);
}
