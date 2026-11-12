// No controller for host tests (platform/input.h): the tests feed game::UpdateMission their own input states.
#include "platform/input.h"

namespace input {

// Always the neutral, disconnected state.
static State state;

void Update()
{
    // The tests script their input directly.
}

const State& Get()
{
    return state;
}

void Rumble(float low, float high, int duration)
{
    (void)low;
    (void)high;
    (void)duration;
}

bool Init()
{
    return true;
}

} // namespace input
