#include "protocol/PacketHandler.h"
#include <iostream>

void PacketHandler::handle(const std::vector<uint8_t>& data) {
    std::cout << "[Protocol] Handling packet of size " << data.size() << " bytes" << std::endl;
    if (data.empty()) return;

    // Basic Minecraft VarInt length-prefixed packet parsing (handshake phase)
    size_t offset = 0;
    auto readVarInt = [&](int& out) -> bool {
        out = 0;
        int numRead = 0;
        uint8_t byte = 0;
        do {
            if (offset >= data.size() || numRead > 5) return false;
            byte = data[offset++];
            out |= (byte & 0x7F) << (7 * numRead);
            numRead++;
        } while (byte & 0x80);
        return true;
    };

    int packetLength = 0;
    if (!readVarInt(packetLength)) {
        std::cout << "[Protocol] Failed to read packet length (VarInt)" << std::endl;
        return;
    }
    int packetId = 0;
    if (!readVarInt(packetId)) {
        std::cout << "[Protocol] Failed to read packet ID (VarInt)" << std::endl;
        return;
    }
    std::cout << "[Protocol] Packet ID: 0x" << std::hex << packetId << std::dec << std::endl;
    // For handshake, packetId should be 0x00
    if (packetId == 0x00) {
        std::cout << "[Protocol] Handshake packet detected (not fully parsed)" << std::endl;
        // TODO: Parse handshake fields (protocol version, server address, port, next state)
    }
    // TODO: Handle other packet types
}
