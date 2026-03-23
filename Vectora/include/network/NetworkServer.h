#pragma once


#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

class NetworkServer {
public:
    NetworkServer();
    ~NetworkServer();
    void start();
private:
#ifdef _WIN32
    WSADATA wsaData;
    SOCKET listenSocket = INVALID_SOCKET;
#endif
};
