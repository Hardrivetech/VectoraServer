#include "network/NetworkServer.h"
#include <iostream>
#ifdef _WIN32
#include <winsock2.h>
#endif
#include "protocol/PacketHandler.h"
#include <vector>
#include <thread>

NetworkServer::NetworkServer() {
#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
    }
#endif
}

NetworkServer::~NetworkServer() {
#ifdef _WIN32
    if (listenSocket != INVALID_SOCKET) {
        closesocket(listenSocket);
    }
    WSACleanup();
#endif
}

void NetworkServer::start() {
    std::cout << "[Network] Server starting on port 25565..." << std::endl;
#ifdef _WIN32
    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        std::cerr << "Failed to create socket." << std::endl;
        return;
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(25565);

    if (bind(listenSocket, (SOCKADDR*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Bind failed." << std::endl;
        closesocket(listenSocket);
        return;
    }

    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "Listen failed." << std::endl;
        closesocket(listenSocket);
        return;
    }

    std::cout << "[Network] Listening for connections..." << std::endl;
    SOCKET clientSocket;
    sockaddr_in clientAddr;
    int clientAddrSize = sizeof(clientAddr);
    auto clientHandler = [](SOCKET clientSocket) {
        std::cout << "[Network] Client connected!" << std::endl;
        PacketHandler handler;
        uint8_t buffer[1024];
        int bytesReceived = recv(clientSocket, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (bytesReceived > 0) {
            std::vector<uint8_t> data(buffer, buffer + bytesReceived);
            handler.handle(data);
        } else {
            std::cout << "[Network] No data received or connection closed." << std::endl;
        }
        closesocket(clientSocket);
    };

    while (true) {
        clientSocket = accept(listenSocket, (SOCKADDR*)&clientAddr, &clientAddrSize);
        if (clientSocket == INVALID_SOCKET) {
            std::cerr << "Accept failed." << std::endl;
            break;
        }
        std::thread(clientHandler, clientSocket).detach();
    }
#else
    std::cout << "[Network] (Non-Windows platforms not yet implemented)" << std::endl;
#endif
}
