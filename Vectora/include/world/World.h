#pragma once

class World {
public:
    void tick();
    // Loads a chunk at (x, z) from the correct region file
    std::shared_ptr<ChunkData> loadChunk(int chunkX, int chunkZ);
};
