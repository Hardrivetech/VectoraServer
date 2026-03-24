#include "world/AnvilRegion.h"
#include "world/NBT.h"
#include "world/ZlibUtil.h"
#include "world/BlockStateParser.h"
#include <iostream>
#include <memory>



std::shared_ptr<Chunk> parseChunk(const std::shared_ptr<ChunkData>& chunkData) {
    if (!chunkData) return nullptr;
    if (chunkData->compressionType != 2) {
        std::cerr << "[ChunkParser] Skipping chunk at " << chunkData->x << "," << chunkData->z << " due to unsupported compression type: " << (int)chunkData->compressionType << std::endl;
        return nullptr;
    }
    std::vector<uint8_t> decompressed;
    if (!decompressZlib(chunkData->rawData, decompressed)) {
        std::cerr << "[ChunkParser] Failed to decompress chunk at " << chunkData->x << "," << chunkData->z << std::endl;
        return nullptr;
    }
    size_t offset = 0;
    auto root = parseNBT(decompressed, offset);
    if (!root) {
        std::cerr << "[ChunkParser] Failed to parse NBT for chunk at " << chunkData->x << "," << chunkData->z << std::endl;
        return nullptr;
    }
    auto chunk = std::make_shared<Chunk>();
    chunk->x = chunkData->x;
    chunk->z = chunkData->z;
    // Parse sections
    auto level = root->children["Level"];
    if (level && level->children.count("Sections")) {
        auto& sectionsList = level->children["Sections"]->list;
        for (auto& sectionTag : sectionsList) {
            BlockSection section;
            section.y = sectionTag->children["Y"]->intValue;
            parseBlockSection(section, sectionTag);
            chunk->sections.push_back(section);
        }
    }
    // Parse entities
    if (level && level->children.count("Entities")) {
        for (auto& ent : level->children["Entities"]->list) {
            chunk->entities.push_back(ent);
        }
    }
    // Parse tile entities
    if (level && level->children.count("TileEntities")) {
        for (auto& te : level->children["TileEntities"]->list) {
            chunk->tileEntities.push_back(te);
        }
    }
    return chunk;
}
