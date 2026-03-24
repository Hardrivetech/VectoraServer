#include "world/NBT.h"
#include <cstring>

static std::string readString(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset + 2 > data.size()) return "";
    uint16_t len = (data[offset] << 8) | data[offset + 1];
    offset += 2;
    if (offset + len > data.size()) return "";
    std::string str(data.begin() + offset, data.begin() + offset + len);
    offset += len;
    return str;
}

std::shared_ptr<NBT> parseNBT(const std::vector<uint8_t>& data, size_t& offset) {
    if (offset >= data.size()) return nullptr;
    NBTType type = static_cast<NBTType>(data[offset++]);
    std::string name = readString(data, offset);
    auto tag = std::make_shared<NBT>();
    tag->type = type;
    tag->name = name;
    switch (type) {
        case NBTType::End:
            break;
        case NBTType::Byte:
            tag->intValue = (int8_t)data[offset++];
            break;
        case NBTType::Short:
            tag->intValue = (int16_t)((data[offset] << 8) | data[offset + 1]);
            offset += 2;
            break;
        case NBTType::Int:
            tag->intValue = (int32_t)((data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3]);
            offset += 4;
            break;
        case NBTType::Long:
            tag->longValue = 0;
            for (int i = 0; i < 8; ++i) tag->longValue = (tag->longValue << 8) | data[offset++];
            break;
        case NBTType::Float:
            std::memcpy(&tag->doubleValue, &data[offset], 4);
            offset += 4;
            break;
        case NBTType::Double:
            std::memcpy(&tag->doubleValue, &data[offset], 8);
            offset += 8;
            break;
        case NBTType::ByteArray: {
            int32_t len = (int32_t)((data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3]);
            offset += 4;
            tag->byteArray.assign(data.begin() + offset, data.begin() + offset + len);
            offset += len;
            break;
        }
        case NBTType::String:
            tag->stringValue = readString(data, offset);
            break;
        case NBTType::List: {
            NBTType elemType = static_cast<NBTType>(data[offset++]);
            int32_t len = (int32_t)((data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3]);
            offset += 4;
            for (int i = 0; i < len; ++i) {
                tag->list.push_back(parseNBT(data, offset));
            }
            break;
        }
        case NBTType::Compound: {
            while (true) {
                auto child = parseNBT(data, offset);
                if (!child || child->type == NBTType::End) break;
                tag->children[child->name] = child;
            }
            break;
        }
        case NBTType::IntArray: {
            int32_t len = (int32_t)((data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3]);
            offset += 4;
            for (int i = 0; i < len; ++i) {
                int32_t val = (int32_t)((data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3]);
                offset += 4;
                tag->intArray.push_back(val);
            }
            break;
        }
        case NBTType::LongArray: {
            int32_t len = (int32_t)((data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3]);
            offset += 4;
            for (int i = 0; i < len; ++i) {
                int64_t val = 0;
                for (int j = 0; j < 8; ++j) val = (val << 8) | data[offset++];
                tag->longArray.push_back(val);
            }
            break;
        }
        default:
            break;
    }
    return tag;
}
