#pragma once
#include <memory>
#include "world/Chunk.h"
#include "world/AnvilRegion.h"

// Parses a ChunkData into a Chunk
std::shared_ptr<Chunk> parseChunk(const std::shared_ptr<ChunkData>& chunkData);
