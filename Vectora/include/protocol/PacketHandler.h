#pragma once


#include <vector>
#include <cstdint>


#include <string>
#ifdef _WIN32
#include <winsock2.h>
#endif


class PacketHandler {
public:
    void handle(const std::vector<uint8_t>& data, void* socketPtr = nullptr);
private:
    void sendLoginSuccess(const std::string& username, void* socketPtr);
    void sendLoginFinished(void* socketPtr);
    void sendJoinGame(const std::string& username, void* socketPtr);
    void sendPlayerPositionAndLook(void* socketPtr);
    void sendKeepAliveResponse(int64_t id, void* socketPtr);
    void sendDisconnect(const std::string& message, void* socketPtr);
    std::vector<uint8_t> encodeVarInt(int value);
    std::vector<uint8_t> encodeString(const std::string& str);
    std::vector<uint8_t> encodeLong(int64_t value);
    std::vector<uint8_t> encodeDouble(double value);
};
