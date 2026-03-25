#pragma once

struct ClientState {
    bool statusState = false;
    // Player position and rotation (for play state)
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool onGround = false;
};
