#pragma once
#include "world/Chunk.h"
#include "world/NBT.h"
#include <memory>

// Helper to parse block states and palette from a section NBT
void parseBlockSection(BlockSection& section, const std::shared_ptr<NBT>& sectionTag);
