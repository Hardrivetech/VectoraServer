#pragma once


#include <vector>
#include <cstdint>

class PacketHandler {
public:
    void handle(const std::vector<uint8_t>& data);
};
