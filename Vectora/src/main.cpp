#include <iostream>
#include "network/NetworkServer.h"

int main() {
    std::cout << "Vectora Minecraft Server (C++) - v0.1" << std::endl;
    NetworkServer server;
    server.start();
    return 0;
}
