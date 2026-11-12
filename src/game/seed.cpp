// Mission seeds (docs/design.md §9): mixing clock readings into a seed (splitmix64), and the settings in autoplay.txt
// (key=value lines; only seed= is known). Hand-rolled parsing: nxdk's strtol and strtof assert on input they
// dislike, and the file is written by hand.
#include "game/game.h"

namespace game {

// --- Helpers

static uint64_t SplitMix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static bool Space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static char Lower(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// True when text[0..length) equals word, ignoring case.
static bool Equals(const char* text, int length, const char* word)
{
    int i = 0;
    for (; i < length && word[i]; i++) {
        if (Lower(text[i]) != word[i])
            return false;
    }
    return i == length && word[i] == '\0';
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = Lower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

// A 32-bit number: 0x and 1-8 hex digits, or 1-10 decimal digits up to 4294967295. False for anything else.
static bool ParseNumber(const char* text, int length, uint32_t& out)
{
    if (length > 2 && text[0] == '0' && Lower(text[1]) == 'x') {
        if (length > 10)
            return false;
        uint32_t value = 0;
        for (int i = 2; i < length; i++) {
            int digit = HexDigit(text[i]);
            if (digit < 0)
                return false;
            value = value << 4 | static_cast<uint32_t>(digit);
        }
        out = value;
        return true;
    }
    if (length < 1 || length > 10)
        return false;
    uint64_t value = 0;
    for (int i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9')
            return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    if (value > 0xFFFFFFFFull)
        return false;
    out = static_cast<uint32_t>(value);
    return true;
}

// The seed= value: "fixed", or a number. Anything else leaves the setting unset (the clock seed).
static void ParseSeed(const char* value, int length, AutoplaySettings& settings)
{
    uint32_t seed = 0;
    if (Equals(value, length, "fixed")) {
        settings.seedSet = true;
        settings.seed = FIXED_SEED;
    } else if (ParseNumber(value, length, seed)) {
        settings.seedSet = true;
        settings.seed = seed;
    }
}

// --- Seeds

uint32_t MixSeed(uint64_t a, uint64_t b)
{
    return static_cast<uint32_t>(SplitMix(SplitMix(a) ^ b) >> 32);
}

AutoplaySettings ParseAutoplaySettings(const char* text, int length)
{
    AutoplaySettings settings;
    int i = 0;
    while (i < length) {
        int end = i;
        while (end < length && text[end] != '\n' && text[end] != '\0')
            end++;
        // One line: "key = value", "# comment" or blank; a # also ends the value.
        int stop = i;
        while (stop < end && text[stop] != '#')
            stop++;
        int equals = i;
        while (equals < stop && text[equals] != '=')
            equals++;
        if (equals < stop) {
            int keyStart = i;
            int keyEnd = equals;
            while (keyStart < keyEnd && Space(text[keyStart]))
                keyStart++;
            while (keyEnd > keyStart && Space(text[keyEnd - 1]))
                keyEnd--;
            int valueStart = equals + 1;
            int valueEnd = stop;
            while (valueStart < valueEnd && Space(text[valueStart]))
                valueStart++;
            while (valueEnd > valueStart && Space(text[valueEnd - 1]))
                valueEnd--;
            if (Equals(text + keyStart, keyEnd - keyStart, "seed"))
                ParseSeed(text + valueStart, valueEnd - valueStart, settings);
        }
        if (end < length && text[end] == '\0')
            break;
        i = end + 1;
    }
    return settings;
}

} // namespace game
