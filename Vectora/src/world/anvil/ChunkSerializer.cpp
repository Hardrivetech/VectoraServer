#include "world/ChunkSerializer.h"
#include <cstring>
#include <iostream>

// Helper to write a VarInt
static void writeVarInt(std::vector<uint8_t>& out, int value) {
    do {
        uint8_t temp = value & 0x7F;
        value >>= 7;
        if (value != 0) temp |= 0x80;
        out.push_back(temp);
    } while (value != 0);
}

// Helper to write a VarLong
static void writeVarLong(std::vector<uint8_t>& out, int64_t value) {
    do {
        uint8_t temp = value & 0x7F;
        value >>= 7;
        if (value != 0) temp |= 0x80;
        out.push_back(temp);
    } while (value != 0);
}

std::vector<uint8_t> serializeChunkData(const std::shared_ptr<Chunk>& chunk, int protocolVersion) {
    std::vector<uint8_t> out;
    // Chunk X, Z
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&chunk->x), reinterpret_cast<const uint8_t*>(&chunk->x) + 4);
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&chunk->z), reinterpret_cast<const uint8_t*>(&chunk->z) + 4);
    // Full chunk (true)
    out.push_back(1);
    // Primary bit mask (all sections present)
    uint64_t mask = 0;
    for (const auto& section : chunk->sections) mask |= (1ULL << (section.y & 0xF));
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(&mask), reinterpret_cast<const uint8_t*>(&mask) + 8);
    // Heightmaps (empty NBT for now)
    out.push_back(10); out.push_back(0); out.push_back(0); out.push_back(0); // TAG_Compound("") + TAG_End
    // Biomes (dummy 1024 ints)
    writeVarInt(out, 1024);
    for (int i = 0; i < 1024; ++i) {
        out.insert(out.end(), {0,0,0,0});
    }
    // Buffer for chunk section data
    std::vector<uint8_t> sectionData;
    for (const auto& section : chunk->sections) {
        // Block count (dummy)
        sectionData.push_back(0);
        sectionData.push_back(0);
        // Bits per block (minimum 4)
        uint8_t bitsPerBlock = 4;
        sectionData.push_back(bitsPerBlock);
        // Palette length
        writeVarInt(sectionData, (int)section.palette.size());
        // Palette entries (VarInt for each block name index, dummy 0)
        for (size_t i = 0; i < section.palette.size(); ++i) writeVarInt(sectionData, (int)i);
        // Data array length (dummy)
        writeVarInt(sectionData, 1);
        // Data array (dummy)
        writeVarLong(sectionData, 0);
    }
    // Write section data length
    writeVarInt(out, (int)sectionData.size());
    out.insert(out.end(), sectionData.begin(), sectionData.end());
    // Block entities (empty)
    writeVarInt(out, 0);
    // Trust edges (true)
    out.push_back(1);
    // Light masks (dummy, all present)
    writeVarInt(out, 0xFFFF);
    writeVarInt(out, 0xFFFF);
    // Empty arrays for light data
    writeVarInt(out, 0);
    writeVarInt(out, 0);
    writeVarInt(out, 0);
    writeVarInt(out, 0);
    return out;
}
