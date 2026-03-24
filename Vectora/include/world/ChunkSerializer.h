#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include "world/Chunk.h"

// Serializes a chunk into the network format for the Chunk Data packet
std::vector<uint8_t> serializeChunkData(const std::shared_ptr<Chunk>& chunk, int protocolVersion = 761);
