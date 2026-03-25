#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <map>

// Minimal NBT tag types
enum class NBTType : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12
};


struct NBT {
    NBTType type;
    std::string name;
    // For compound/list types
    std::map<std::string, std::shared_ptr<NBT>> children;
    std::vector<std::shared_ptr<NBT>> list;
    // For primitive types
    int32_t intValue = 0;
    int64_t longValue = 0;
    double doubleValue = 0.0;
    std::string stringValue;
    std::vector<uint8_t> byteArray;
    std::vector<int32_t> intArray;
    std::vector<int64_t> longArray;

    // Encode this NBT tag (and children) to a byte array
    std::vector<uint8_t> encode() const;
};

std::shared_ptr<NBT> parseNBT(const std::vector<uint8_t>& data, size_t& offset);
