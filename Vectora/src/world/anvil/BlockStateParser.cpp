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
        // TODO: Unpack bits according to palette size
        // For now, just store raw values
        for (auto val : arr) {
            section.blockStates.push_back(static_cast<uint16_t>(val & 0xFFFF));
        }
    }
}
