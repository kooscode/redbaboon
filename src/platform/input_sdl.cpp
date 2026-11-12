// Controller input through SDL3's gamepad API (nxdk-sdl3 maps the Duke and the S controller to it): the first
// connected pad, with a radial dead zone on both sticks (creative brief §5: worn sticks drift), triggers 0..1,
// buttons with this frame's presses, and rumble. SDL_Init(SDL_INIT_GAMEPAD) is done by main.cpp.
#include "platform/input.h"

#include <SDL3/SDL.h>
#include <windows.h>

#include <math.h>
#include <stdio.h>

namespace input {

constexpr float DEAD_ZONE = 0.24f;
constexpr float AXIS_SCALE = 1.0f / 32767.0f;

// SDL buttons for each input::Button (Black and White are the right and left shoulders in nxdk-sdl3's mapping).
constexpr SDL_GamepadButton BUTTON_MAP[static_cast<int>(Button::COUNT)] = {
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};

// The open controller (nullptr when none is connected) and this frame's state.
static SDL_Gamepad* pad = nullptr;
static bool rumbleLogged = false;   // the first rumble's result has gone to the debug log
static State state;

// --- Helpers

static float Axis(SDL_GamepadAxis axis)
{
    float v = SDL_GetGamepadAxis(pad, axis) * AXIS_SCALE;
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

// Radial dead zone: inside it the stick reads zero; outside, the rest of the travel is rescaled to 0..1.
static void Stick(SDL_GamepadAxis axisX, SDL_GamepadAxis axisY, float& x, float& y)
{
    float rx = Axis(axisX);
    float ry = Axis(axisY);
    float length = sqrtf(rx * rx + ry * ry);
    if (length <= DEAD_ZONE) {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    float scaled = (length - DEAD_ZONE) / (1.0f - DEAD_ZONE);
    if (scaled > 1.0f)
        scaled = 1.0f;
    x = rx / length * scaled;
    y = ry / length * scaled;
}

// One line to the debug output (the serial log on the console).
static void Log(const char* what, const char* detail)
{
    char line[128];
    snprintf(line, sizeof(line), "RedBaboon input: %s %s\n", what, detail ? detail : "");
    OutputDebugStringA(line);
}

static void Open(SDL_JoystickID id)
{
    pad = SDL_OpenGamepad(id);
    if (pad)
        Log("pad connected:", SDL_GetGamepadName(pad));
}

static void OpenFirst()
{
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count && !pad; i++)
        Open(ids[i]);
    SDL_free(ids);
}

// --- Input

bool Init()
{
    OpenFirst();
    return true;
}

void Update()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
            Open(event.gdevice.which);
        } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == event.gdevice.which) {
            SDL_CloseGamepad(pad);
            pad = nullptr;
            Log("pad disconnected", nullptr);
            OpenFirst();
        }
    }
    bool before[static_cast<int>(Button::COUNT)];
    for (int i = 0; i < static_cast<int>(Button::COUNT); i++)
        before[i] = state.down[i];
    state = State();
    if (!pad)
        return;
    state.connected = true;
    Stick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, state.leftX, state.leftY);
    Stick(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, state.rightX, state.rightY);
    state.leftTrigger = Axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    state.rightTrigger = Axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    if (state.leftTrigger < 0.0f)
        state.leftTrigger = 0.0f;
    if (state.rightTrigger < 0.0f)
        state.rightTrigger = 0.0f;
    for (int i = 0; i < static_cast<int>(Button::COUNT); i++) {
        state.down[i] = SDL_GetGamepadButton(pad, BUTTON_MAP[i]);
        state.pressed[i] = state.down[i] && !before[i];
    }
}

const State& Get()
{
    return state;
}

void Rumble(float low, float high, int duration)
{
    if (!pad)
        return;
    auto level = [](float v) {
        float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<Uint16>(c * 65535.0f);
    };
    bool ok = SDL_RumbleGamepad(pad, level(low), level(high), static_cast<Uint32>(duration));
    if (!rumbleLogged) {
        rumbleLogged = true;
        Log(ok ? "rumble ok" : "rumble failed:", ok ? nullptr : SDL_GetError());
    }
}

} // namespace input
