#pragma once
#include <vector>
#include <cstdint>

bool decompressZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);
