#include "world/World.h"
#include <iostream>


#include "world/AnvilRegion.h"
#include <memory>
#include <sstream>

void World::tick() {
    std::cout << "[World] Ticking world... (placeholder)" << std::endl;
    // TODO: Implement world logic
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
