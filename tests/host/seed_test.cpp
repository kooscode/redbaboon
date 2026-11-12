// Mission seeds (game/seed.cpp): autoplay.txt's settings parser (seed=fixed, hex, decimal, comments, junk, unknown
// keys, Windows line ends) and the clock seed mixing.
#include "game/game.h"

#include <stdio.h>
#include <string.h>

using namespace game;

static int failures = 0;

static void Check(bool ok, const char* what)
{
    printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

// The settings of a file's text.
static AutoplaySettings Parse(const char* text)
{
    return ParseAutoplaySettings(text, static_cast<int>(strlen(text)));
}

static bool Seeds(const char* text, uint32_t seed)
{
    AutoplaySettings s = Parse(text);
    return s.seedSet && s.seed == seed;
}

static bool Clock(const char* text)
{
    return !Parse(text).seedSet;
}

int main()
{
    printf("autoplay.txt settings\n");
    Check(Clock(""), "empty file: clock seed");
    Check(Clock("1\n"), "old \"1\" file: clock seed");
    Check(Seeds("seed=fixed", FIXED_SEED), "seed=fixed");
    Check(Seeds("  SEED = Fixed  \r\n", FIXED_SEED), "spaces, case and CRLF");
    Check(Seeds("seed=0x1234ABCD\n", 0x1234ABCDu), "hex");
    Check(Seeds("seed=0XffffFFFF", 0xFFFFFFFFu), "hex, upper X, all bits");
    Check(Seeds("seed=4294967295", 0xFFFFFFFFu), "largest decimal");
    Check(Seeds("seed=0", 0u), "zero");
    Check(Clock("seed=4294967296"), "decimal too big: ignored");
    Check(Clock("seed=0x123456789"), "hex too long: ignored");
    Check(Clock("seed=12ab"), "junk: ignored");
    Check(Clock("seed=0x"), "hex without digits: ignored");
    Check(Clock("seed="), "empty value: ignored");
    Check(Clock("# seed=fixed\n"), "commented out");
    Check(Seeds("seed=42 # the answer", 42u), "comment after the value");
    Check(Seeds("speed=9\nfoo\n\nseed=7\n", 7u), "unknown keys and lines skipped");
    Check(Seeds("seed=1\nseed=2", 2u), "the last seed line wins");
    const char nul[] = "seed=5\0seed=6";
    Check(ParseAutoplaySettings(nul, sizeof(nul) - 1).seed == 5u, "stops at a NUL byte");

    printf("clock seeds\n");
    Check(MixSeed(1, 2) != MixSeed(1, 3) && MixSeed(1, 2) != MixSeed(2, 2), "different readings, different seeds");
    Check(MixSeed(7, 9) == MixSeed(7, 9), "same readings, same seed");

    printf(failures ? "FAILED (%d)\n" : "PASSED (0 failed)\n", failures);
    return failures ? 1 : 0;
}
