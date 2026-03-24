#include "world/Chunk.h"
#include <iostream>
#include <vector>
#include <string>
#include <memory>

// Helper to parse block states and palette from a section NBT
void parseBlockSection(BlockSection& section, const std::shared_ptr<NBT>& sectionTag) {
    // Parse palette
    if (sectionTag->children.count("Palette")) {
        for (auto& entry : sectionTag->children["Palette"]->list) {
            if (entry->children.count("Name"))
                section.palette.push_back(entry->children["Name"]->stringValue);
            else if (!entry->stringValue.empty())
                section.palette.push_back(entry->stringValue);
            else
                section.palette.push_back("");
        }
    }
    // Parse block states (bit-packed array)
    if (sectionTag->children.count("BlockStates")) {
        const auto& arr = sectionTag->children["BlockStates"]->longArray;
        size_t paletteSize = section.palette.size();
        if (paletteSize > 0 && !arr.empty()) {
            // Each block uses N bits, where N = ceil(log2(paletteSize))
            int bitsPerBlock = 4;
            while ((1U << bitsPerBlock) < paletteSize) ++bitsPerBlock;
            int blocksPerLong = 64 / bitsPerBlock;
            int blockCount = 4096; // 16x16x16
            section.blockStates.clear();
            section.blockStates.reserve(blockCount);
            int bitIndex = 0;
            for (int i = 0; i < blockCount; ++i) {
                int longIdx = bitIndex / 64;
                int startBit = bitIndex % 64;
                uint64_t value = arr[longIdx];
                uint64_t mask = (1ULL << bitsPerBlock) - 1;
                uint16_t paletteIdx;
                if (startBit + bitsPerBlock <= 64) {
                    paletteIdx = (value >> startBit) & mask;
                } else {
                    // Value spans two longs
                    int bitsInFirst = 64 - startBit;
                    int bitsInSecond = bitsPerBlock - bitsInFirst;
                    uint64_t nextValue = (longIdx + 1 < arr.size()) ? arr[longIdx + 1] : 0;
                    paletteIdx = ((value >> startBit) | (nextValue << bitsInFirst)) & mask;
                }
                section.blockStates.push_back(paletteIdx);
                bitIndex += bitsPerBlock;
            }
        }
    }
}
