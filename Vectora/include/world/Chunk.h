#pragma once
#include <vector>
#include <memory>
#include <cstdint>
#include <string>
#include "world/NBT.h"

struct BlockSection {
    int y;
    std::vector<uint16_t> blockStates;
    std::vector<std::string> palette;
    // TODO: Lighting, biome, etc.
};

struct Chunk {
    int x, z;
    std::vector<BlockSection> sections;
    std::vector<std::shared_ptr<NBT>> entities;
    std::vector<std::shared_ptr<NBT>> tileEntities;
};
