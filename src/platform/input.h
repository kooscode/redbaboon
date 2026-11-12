// Controller input: the first connected gamepad, read through SDL3 (input_sdl.cpp).
#pragma once

namespace input {

enum class Button {
    A,
    B,
    X,
    Y,
    BLACK,
    WHITE,
    START,
    BACK,
    LEFT_STICK,
    RIGHT_STICK,
    DPAD_UP,
    DPAD_DOWN,
    DPAD_LEFT,
    DPAD_RIGHT,
    COUNT
};

struct State {
    float leftX = 0.0f;  // -1 left .. 1 right (dead zone applied)
    float leftY = 0.0f;  // -1 up .. 1 down (stick pushed forward is negative)
    float rightX = 0.0f;
    float rightY = 0.0f;
    float leftTrigger = 0.0f;  // 0..1
    float rightTrigger = 0.0f; // 0..1
    bool down[static_cast<int>(Button::COUNT)] = {};
    bool pressed[static_cast<int>(Button::COUNT)] = {}; // went down this frame
    bool connected = false;
};

// Polls SDL events and the controller. Call once per frame.
void Update();

const State& Get();

// Rumble, 0..1 for each motor, for duration milliseconds.
void Rumble(float low, float high, int duration);

bool Init();

} // namespace input
