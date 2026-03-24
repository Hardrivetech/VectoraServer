#include <string>
#include "protocol/PacketHandler.h"
#include <iostream>
#include "world/AnvilRegion.h"
#include "world/ChunkParser.h"
#include "world/ChunkSerializer.h"


void PacketHandler::handle(const std::vector<uint8_t>& data, void* socketPtr) {
    std::cout << "[Protocol] Handling packet of size " << data.size() << " bytes" << std::endl;
    if (data.empty()) return;
    // Make a non-const copy so we can erase processed bytes
    std::vector<uint8_t>& buffer = const_cast<std::vector<uint8_t>&>(data);

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
    auto readString = [&](std::string& out) -> bool {
        int len = 0;
        if (!readVarInt(len)) return false;
        if (offset + len > data.size()) return false;
        out = std::string(data.begin() + offset, data.begin() + offset + len);
        offset += len;
        return true;
    };
    auto readUShort = [&]() -> uint16_t {
        if (offset + 2 > data.size()) return 0;
        uint16_t val = (data[offset] << 8) | data[offset + 1];
        offset += 2;
        return val;
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
    size_t packet_start = 0;
    std::cout << "[Protocol] Packet ID: 0x" << std::hex << packetId << std::dec << std::endl;
    if (packetId != 0x00) {
        std::cout << "[Protocol] Received non-handshake packet ID: 0x" << std::hex << packetId << std::dec << std::endl;
    }

    if (packetId == 0x00) {
        // Handshake or login start
        if (offset < data.size()) {
            // Try to read as handshake (protocol version is VarInt, server address is String, port is UShort, next state is VarInt)
            int protocolVersion = 0;
            size_t save_offset = offset;
            if (readVarInt(protocolVersion)) {
                std::string serverAddress;
                if (readString(serverAddress)) {
                    uint16_t serverPort = readUShort();
                    int nextState = 0;
                    if (readVarInt(nextState)) {
                        std::cout << "[Protocol] Handshake: protocolVersion=" << protocolVersion
                                  << ", serverAddress=" << serverAddress
                                  << ", serverPort=" << serverPort
                                  << ", nextState=" << nextState << std::endl;
                        // TODO: Store state for login/status
                        // Erase processed bytes (handshake packet)
                        buffer.erase(buffer.begin(), buffer.begin() + offset);
                        return;
                    }
                }
            }
            // If not handshake, reset offset and try login start
            offset = save_offset;
            std::string username;
            if (readString(username)) {
                std::cout << "[Protocol] Login Start: username=" << username << std::endl;
                // Proceed to Play state: send Login Success, Join Game, Player Position, and Chunk Data

                this->sendLoginSuccess(username, socketPtr);
                this->sendLoginFinished(socketPtr);
                this->sendJoinGame(username, socketPtr);
                this->sendPlayerPositionAndLook(socketPtr);


                // --- Chunk sending logic ---
                // Load region and chunk (hardcoded to 0,0 for demo)
                std::string regionPath = "world/region/r.0.0.mca";
                AnvilRegion region(regionPath);
                if (region.isValid()) {
                    auto chunkData = region.loadChunk(0, 0);
                    if (chunkData) {
                        auto chunk = parseChunk(chunkData);
                        if (chunk) {
                            auto chunkPacket = serializeChunkData(chunk);
                            // Prepend Chunk Data packet ID (0x22 for 1.21.11)
                            auto pid = encodeVarInt(0x22);
                            chunkPacket.insert(chunkPacket.begin(), pid.begin(), pid.end());
                            // Prepend length
                            auto plen = encodeVarInt((int)chunkPacket.size());
                            chunkPacket.insert(chunkPacket.begin(), plen.begin(), plen.end());
                            SOCKET sock = *(SOCKET*)socketPtr;
                            send(sock, reinterpret_cast<const char*>(chunkPacket.data()), (int)chunkPacket.size(), 0);
                            std::cout << "[Protocol] Sent Chunk Data (0,0)" << std::endl;
                        } else {
                            std::cout << "[Protocol] Failed to parse chunk (0,0)" << std::endl;
                        }
                    } else {
                        std::cout << "[Protocol] Failed to load chunk data (0,0)" << std::endl;
                    }
                } else {
                    std::cout << "[Protocol] Region file not valid: " << regionPath << std::endl;
                }
                // Erase processed bytes (login start packet)
                buffer.erase(buffer.begin(), buffer.begin() + offset);
                return;
            } else {
                std::cout << "[Protocol] Did not parse Login Start after handshake. Data left: " << (data.size() - offset) << " bytes." << std::endl;
            }
            // Erase processed bytes (failed login start attempt)
            buffer.erase(buffer.begin(), buffer.begin() + offset);
            return;
        }

    } else if (packetId == 0x21) { // Keep Alive (clientbound is 0x21, serverbound is 0x0F in 1.19+)
        // Try to read keep-alive ID (long)
        if (offset + 8 <= data.size()) {
            int64_t keepAliveId = 0;
            for (int i = 0; i < 8; ++i) {
                keepAliveId = (keepAliveId << 8) | data[offset++];
            }
            std::cout << "[Protocol] Received Keep Alive, echoing back ID: " << keepAliveId << std::endl;
            sendKeepAliveResponse(keepAliveId, socketPtr);
            // Erase processed bytes (keep alive)
            buffer.erase(buffer.begin(), buffer.begin() + offset);
            return;
        }
    }
    std::cout << "[Protocol] Unhandled packet ID: 0x" << std::hex << packetId << std::dec << std::endl;
    // Erase processed bytes (unhandled packet)
    buffer.erase(buffer.begin(), buffer.begin() + offset);
}

void PacketHandler::sendDisconnect(const std::string& message, void* socketPtr) {
#ifdef _WIN32
    if (!socketPtr) return;
    SOCKET sock = *(SOCKET*)socketPtr;
    // Disconnect packet (Login state: 0x00)
    std::vector<uint8_t> packet;
    std::vector<uint8_t> pid = encodeVarInt(0x00); // Packet ID for Disconnect (login)
    packet.insert(packet.end(), pid.begin(), pid.end());
    // Reason (chat, as JSON string)
    std::string safeMsg = message;
    if (safeMsg.empty()) safeMsg = "{\"text\":\"Disconnected by server\"}";
    std::vector<uint8_t> reason = encodeString(safeMsg);
    packet.insert(packet.end(), reason.begin(), reason.end());
    // Prepend length
    std::vector<uint8_t> plen = encodeVarInt((int)packet.size());
    packet.insert(packet.begin(), plen.begin(), plen.end());
    // Send
    // Log raw packet bytes for debugging
    std::cout << "[Protocol] Disconnect packet bytes: ";
    for (size_t i = 0; i < packet.size(); ++i) {
        printf("%02X ", packet[i]);
    }
    std::cout << std::endl;
    int sent = send(sock, reinterpret_cast<const char*>(packet.data()), (int)packet.size(), 0);
    std::cout << "[Protocol] Sent Disconnect message: " << safeMsg << " (" << sent << " bytes)" << std::endl;
    // Wait longer to ensure packet is sent before closing
    Sleep(500);
    closesocket(sock);
#endif
}

void PacketHandler::sendPlayerPositionAndLook(void* socketPtr) {
#ifdef _WIN32
    if (!socketPtr) return;
    SOCKET sock = *(SOCKET*)socketPtr;
    std::vector<uint8_t> packet;
    auto pid = encodeVarInt(0x39); // Packet ID for Player Position and Look (serverbound 0x14, clientbound 0x39 in 1.19+)
    packet.insert(packet.end(), pid.begin(), pid.end());
    // X, Y, Z (double)
    auto x = encodeDouble(0.0); packet.insert(packet.end(), x.begin(), x.end());
    auto y = encodeDouble(64.0); packet.insert(packet.end(), y.begin(), y.end());
    auto z = encodeDouble(0.0); packet.insert(packet.end(), z.begin(), z.end());
    // Yaw, Pitch (float, but protocol uses double for 1.19+)
    auto yaw = encodeDouble(0.0); packet.insert(packet.end(), yaw.begin(), yaw.end());
    auto pitch = encodeDouble(0.0); packet.insert(packet.end(), pitch.begin(), pitch.end());
    // Flags (byte)
    packet.push_back(0x00);
    // Teleport ID (VarInt)
    auto tid = encodeVarInt(1); packet.insert(packet.end(), tid.begin(), tid.end());
    // Dismount vehicle (bool)
    packet.push_back(0x00);
    // Prepend length
    auto plen = encodeVarInt((int)packet.size());
    packet.insert(packet.begin(), plen.begin(), plen.end());
    send(sock, reinterpret_cast<const char*>(packet.data()), (int)packet.size(), 0);
    std::cout << "[Protocol] Sent Player Position and Look" << std::endl;
    // Debug: print raw bytes
    std::cout << "[Protocol] Player Position and Look packet bytes: ";
    for (size_t i = 0; i < packet.size(); ++i) {
        printf("%02X ", packet[i]);
    }
    std::cout << std::endl;
#endif
}

void PacketHandler::sendKeepAliveResponse(int64_t id, void* socketPtr) {
#ifdef _WIN32
    if (!socketPtr) return;
    SOCKET sock = *(SOCKET*)socketPtr;
    std::vector<uint8_t> packet;
    auto pid = encodeVarInt(0x21); // Packet ID for Keep Alive (clientbound 0x21, serverbound 0x0F in 1.19+)
    packet.insert(packet.end(), pid.begin(), pid.end());
    auto kid = encodeLong(id);
    packet.insert(packet.end(), kid.begin(), kid.end());
    auto plen = encodeVarInt((int)packet.size());
    packet.insert(packet.begin(), plen.begin(), plen.end());
    send(sock, reinterpret_cast<const char*>(packet.data()), (int)packet.size(), 0);
    std::cout << "[Protocol] Sent Keep Alive response" << std::endl;
#endif
}

std::vector<uint8_t> PacketHandler::encodeDouble(double value) {
    std::vector<uint8_t> out(8);
    uint64_t asInt;
    std::memcpy(&asInt, &value, sizeof(double));
    for (int i = 7; i >= 0; --i) {
        out[i] = asInt & 0xFF;
        asInt >>= 8;
    }
    return out;
}

void PacketHandler::sendJoinGame(const std::string& username, void* socketPtr) {
#ifdef _WIN32
    if (!socketPtr) return;
    SOCKET sock = *(SOCKET*)socketPtr;
    // Join Game packet (0x2D for 1.21.11)
    std::vector<uint8_t> packet;
    auto pid = encodeVarInt(0x2D); // Packet ID for Join Game (1.21.11)
    packet.insert(packet.end(), pid.begin(), pid.end());
    // Entity ID (int)
    auto eid = encodeVarInt(1); // Always 1 for demo
    packet.insert(packet.end(), eid.begin(), eid.end());
    // Is hardcore (bool)
    packet.push_back(0x00);
    // Gamemode (byte)
    packet.push_back(0x01); // Creative
    // Previous gamemode (byte)
    packet.push_back(0xFF);
    // World count (VarInt)
    auto wcount = encodeVarInt(1);
    packet.insert(packet.end(), wcount.begin(), wcount.end());
    // World name (String)
    auto wname = encodeString("minecraft:overworld");
    packet.insert(packet.end(), wname.begin(), wname.end());
    // Dimension codec (NBT, minimal valid payload for Overworld/Nether/End)
    // This is a minimal NBT blob for the registry data. In production, use a real NBT encoder.
    // For now, use a hardcoded minimal valid registry for Overworld only.
    std::vector<uint8_t> minimalNbt = {
        0x0A, 0x00, 0x00, // TAG_Compound("")
        0x0A, 0x00, 0x08, 'd','i','m','e','n','s','i','o','n','s', // TAG_Compound("dimensions")
        0x0A, 0x00, 0x09, 'm','i','n','e','c','r','a','f','t',':','o','v','e','r','w','o','r','l','d', // TAG_Compound("minecraft:overworld")
        0x00, // TAG_End
        0x00, // TAG_End
        0x00  // TAG_End
    };
    // NBT is sent as a length-prefixed byte array (VarInt length)
    auto nbtLen = encodeVarInt((int)minimalNbt.size());
    packet.insert(packet.end(), nbtLen.begin(), nbtLen.end());
    packet.insert(packet.end(), minimalNbt.begin(), minimalNbt.end());
    // Dimension (String)
    auto dim = encodeString("minecraft:overworld");
    packet.insert(packet.end(), dim.begin(), dim.end());
    // World name again (String)
    packet.insert(packet.end(), wname.begin(), wname.end());
    // Hashed seed (long)
    auto hseed = encodeLong(0);
    packet.insert(packet.end(), hseed.begin(), hseed.end());
    // Max players (VarInt, ignored by client)
    auto maxp = encodeVarInt(20);
    packet.insert(packet.end(), maxp.begin(), maxp.end());
    // View distance (VarInt)
    auto vdist = encodeVarInt(10);
    packet.insert(packet.end(), vdist.begin(), vdist.end());
    // Simulation distance (VarInt)
    auto sdist = encodeVarInt(10);
    packet.insert(packet.end(), sdist.begin(), sdist.end());
    // Reduced debug info (bool)
    packet.push_back(0x00);
    // Enable respawn screen (bool)
    packet.push_back(0x01);
    // Is debug (bool)
    packet.push_back(0x00);
    // Is flat (bool)
    packet.push_back(0x00);
    // Portal cooldown (VarInt, new in 1.20.2+)
    auto portalCooldown = encodeVarInt(0);
    packet.insert(packet.end(), portalCooldown.begin(), portalCooldown.end());
    // Prepend length
    auto plen = encodeVarInt((int)packet.size());
    packet.insert(packet.begin(), plen.begin(), plen.end());
    // Send
    send(sock, reinterpret_cast<const char*>(packet.data()), (int)packet.size(), 0);
    // Debug: print raw bytes
    std::cout << "[Protocol] Join Game packet bytes: ";
    for (size_t i = 0; i < packet.size(); ++i) {
        printf("%02X ", packet[i]);
    }
    std::cout << std::endl;
    std::cout << "[Protocol] Sent Join Game to " << username << std::endl;
#endif
}

std::vector<uint8_t> PacketHandler::encodeLong(int64_t value) {
    std::vector<uint8_t> out(8);
    for (int i = 7; i >= 0; --i) {
        out[i] = value & 0xFF;
        value >>= 8;
    }
    return out;
}

void PacketHandler::sendLoginSuccess(const std::string& username, void* socketPtr) {
#ifdef _WIN32
    if (!socketPtr) return;
    SOCKET sock = *(SOCKET*)socketPtr;
    // Login Success packet: 0x02 (packet id), UUID (string), username (string)
    std::string fakeUUID = "00000000-0000-0000-0000-000000000000";
    std::vector<uint8_t> packet;
    // Packet ID
    auto pid = encodeVarInt(0x02);
    packet.insert(packet.end(), pid.begin(), pid.end());
    // UUID
    auto uuid = encodeString(fakeUUID);
    packet.insert(packet.end(), uuid.begin(), uuid.end());
    // Username
    auto uname = encodeString(username);
    packet.insert(packet.end(), uname.begin(), uname.end());
    // Prepend length
    auto plen = encodeVarInt((int)packet.size());
    packet.insert(packet.begin(), plen.begin(), plen.end());
    // Send
    send(sock, reinterpret_cast<const char*>(packet.data()), (int)packet.size(), 0);
    std::cout << "[Protocol] Sent Login Success to " << username << std::endl;
#endif
}

std::vector<uint8_t> PacketHandler::encodeVarInt(int value) {
    std::vector<uint8_t> out;
    do {
        uint8_t temp = value & 0x7F;
        value >>= 7;
        if (value != 0) temp |= 0x80;
        out.push_back(temp);
    } while (value != 0);
    return out;
}

std::vector<uint8_t> PacketHandler::encodeString(const std::string& str) {
    std::vector<uint8_t> out = encodeVarInt((int)str.size());
    out.insert(out.end(), str.begin(), str.end());
    return out;
}

void PacketHandler::sendLoginFinished(void* socketPtr) {
#ifdef _WIN32
    if (!socketPtr) return;
    SOCKET sock = *(SOCKET*)socketPtr;
    // Login Finished packet: 0x2A (packet id) for 1.21+
    std::vector<uint8_t> packet;
    auto pid = encodeVarInt(0x2A); // Update if protocol version changes
    packet.insert(packet.end(), pid.begin(), pid.end());
    // Prepend length (should be the size of the packet id only)
    auto plen = encodeVarInt((int)packet.size());
    packet.insert(packet.begin(), plen.begin(), plen.end());
    // Debug: print raw bytes
    std::cout << "[Protocol] Login Finished packet bytes: ";
    for (size_t i = 0; i < packet.size(); ++i) {
        printf("%02X ", packet[i]);
    }
    std::cout << std::endl;
    send(sock, reinterpret_cast<const char*>(packet.data()), (int)packet.size(), 0);
    std::cout << "[Protocol] Sent Login Finished" << std::endl;
#endif
}
