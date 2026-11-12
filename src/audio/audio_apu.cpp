// Output through the MCPX APU's voice processor: one 2D voice playing a looping 48 kHz stereo 16-bit buffer to
// mixbins 0 and 1, which the device keeps filled ahead of the voice's play position. xemu sends what the voice
// processor makes to the host ("monitor" with its "use DSP" setting off, its default), but not what a title writes
// to the AC'97 PCM out, so this is how the game is heard in xemu. On a console the voice would go to the GP DSP,
// which nothing has loaded, so the device uses this path only in xemu (audio_xbox.cpp decides).
//
// Registers and voice layout as xemu emulates them (hw/xbox/mcpx/apu: apu_regs.h, vp/vp.c): the voice structures
// and the scatter-gather table live in contiguous memory the APU reads; the voice is set up with front-end methods
// written to the voice processor's PIO window.
#include "audio/apu.h"

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

namespace audio {

// --- Registers

// The APU's registers (PCI BAR0) and its voice processor PIO window.
static volatile uint32_t* const APU = reinterpret_cast<volatile uint32_t*>(0xFE800000u);
constexpr uint32_t PIO = 0x20000;

// Setup engine control (a non-zero counter mode lets the APU process frames), the voice array and the
// scatter-gather table addresses, and the heads of the three voice lists.
constexpr uint32_t SECTL = 0x2000;
constexpr uint32_t SECTL_XCNTMODE_RUN = 2 << 3;
constexpr uint32_t VPVADDR = 0x202C;
constexpr uint32_t VPSGEADDR = 0x2030;
constexpr uint32_t TVL2D = 0x2054;
constexpr uint32_t TVL3D = 0x2060;
constexpr uint32_t TVLMP = 0x206C;
constexpr uint32_t NO_VOICE = 0xFFFF;

// Front-end methods (offsets in the PIO window).
constexpr uint32_t SET_ANTECEDENT_VOICE = 0x120;
constexpr uint32_t ANTECEDENT_LIST_2D_TOP = 1 << 16;
constexpr uint32_t VOICE_ON = 0x124;
constexpr uint32_t SET_CURRENT_VOICE = 0x2F8;
constexpr uint32_t SET_VOICE_CFG_VBIN = 0x300;
constexpr uint32_t SET_VOICE_CFG_FMT = 0x304;
constexpr uint32_t SET_VOICE_CFG_ENV0 = 0x308;
constexpr uint32_t SET_VOICE_CFG_MISC = 0x318;
constexpr uint32_t SET_VOICE_TAR_VOLA = 0x360;
constexpr uint32_t SET_VOICE_TAR_VOLB = 0x364;
constexpr uint32_t SET_VOICE_TAR_VOLC = 0x368;
constexpr uint32_t SET_VOICE_TAR_PITCH = 0x37C;
constexpr uint32_t SET_VOICE_CFG_BUF_BASE = 0x3A0;
constexpr uint32_t SET_VOICE_CFG_BUF_LBO = 0x3A4;
constexpr uint32_t SET_VOICE_BUF_CBO = 0x3D8;
constexpr uint32_t SET_VOICE_CFG_BUF_EBO = 0x3DC;

// The voice: the first 2D voice (0-63 are 3D voices, whose first bins go through the HRTF submixes).
constexpr uint32_t VOICE = 64;
constexpr uint32_t VOICE_SIZE = 0x80;

// Voice format: stereo, looping, 16-bit samples in 16-bit containers, a block of two samples (one stereo frame:
// the voice steps through its buffer a block at a time).
constexpr uint32_t FMT_TWO_SAMPLES_PER_BLOCK = 1u << 16;
constexpr uint32_t FMT_LOOP = 1u << 25;
constexpr uint32_t FMT_STEREO = 1u << 27;
constexpr uint32_t FMT_S16 = 1u << 28;
constexpr uint32_t FMT_CONTAINER_16 = 1u << 30;

// Volumes: bins 0 and 1 (left, right) at 0 dB, the other six silent (0xFFF), packed as the voice stores them.
constexpr uint32_t VOLA = 0x000F000F;
constexpr uint32_t VOLB = 0xFFFFFFFF;
constexpr uint32_t VOLC = 0xFFFFFFFF;

// Bins: voice bin 0 to mixbin 0 (front left), voice bin 1 to mixbin 1 (front right).
constexpr uint32_t VBIN = (0 << 0) | (1 << 5);

// The play position (current buffer offset) in the voice structure.
constexpr uint32_t VOICE_PAR_OFFSET = 0x58;
constexpr uint32_t CBO_MASK = 0x00FFFFFF;

// --- State

// Pages of the ring (16 KB: 4096 frames of 4 bytes).
constexpr int RING_BYTES = APU_RING_FRAMES * 4;
constexpr int PAGE = 4096;
constexpr int RING_PAGES = RING_BYTES / PAGE;

// The voice array (voices 0..VOICE), the scatter-gather table (one entry per ring page) and the ring; and the next
// frame to write.
static uint8_t* voices = nullptr;
static uint32_t* sgeTable = nullptr;
static int16_t* ring = nullptr;
static int writeFrame = 0;

// Times the voice had run past the written frames.
static int apuCatchUps = 0;

// Writes an APU register.
static void Write(uint32_t offset, uint32_t value)
{
    APU[offset / 4] = value;
}

// Sends a front-end method (xemu runs it at once).
static void Method(uint32_t method, uint32_t argument)
{
    Write(PIO + method, argument);
}

// Zeroed, page-aligned, physically contiguous memory the APU can read.
static void* Contiguous(uint32_t bytes)
{
    void* memory = MmAllocateContiguousMemoryEx(bytes, 0, 0xFFFFFFFF, PAGE, PAGE_READWRITE | PAGE_NOCACHE);
    if (memory)
        RtlZeroMemory(memory, bytes);
    return memory;
}

// --- Output

bool ApuOpen()
{
    voices = static_cast<uint8_t*>(Contiguous((VOICE + 1) * VOICE_SIZE));
    sgeTable = static_cast<uint32_t*>(Contiguous(PAGE));
    ring = static_cast<int16_t*>(Contiguous(RING_BYTES));
    if (!voices || !sgeTable || !ring)
        return false;
    for (int p = 0; p < RING_PAGES; ++p) {
        sgeTable[2 * p] = (uint32_t)MmGetPhysicalAddress(reinterpret_cast<uint8_t*>(ring) + p * PAGE);
        sgeTable[2 * p + 1] = 0;
    }

    // Empty voice lists first: a list head of 0 would make the APU walk voice 0.
    Write(TVL2D, NO_VOICE);
    Write(TVL3D, NO_VOICE);
    Write(TVLMP, NO_VOICE);
    Write(VPVADDR, (uint32_t)MmGetPhysicalAddress(voices));
    Write(VPSGEADDR, (uint32_t)MmGetPhysicalAddress(sgeTable));

    Method(SET_CURRENT_VOICE, VOICE);
    Method(SET_VOICE_CFG_VBIN, VBIN);
    Method(SET_VOICE_CFG_FMT, FMT_TWO_SAMPLES_PER_BLOCK | FMT_LOOP | FMT_STEREO | FMT_S16 | FMT_CONTAINER_16);
    Method(SET_VOICE_CFG_ENV0, 0);
    Method(SET_VOICE_CFG_MISC, 0);
    Method(SET_VOICE_TAR_VOLA, VOLA);
    Method(SET_VOICE_TAR_VOLB, VOLB);
    Method(SET_VOICE_TAR_VOLC, VOLC);
    Method(SET_VOICE_TAR_PITCH, 0);
    Method(SET_VOICE_CFG_BUF_BASE, 0);
    Method(SET_VOICE_CFG_BUF_LBO, 0);
    Method(SET_VOICE_CFG_BUF_EBO, APU_RING_FRAMES - 1);
    Method(SET_VOICE_BUF_CBO, 0);
    Method(SET_ANTECEDENT_VOICE, ANTECEDENT_LIST_2D_TOP);
    Method(VOICE_ON, VOICE);
    Write(SECTL, SECTL_XCNTMODE_RUN);
    writeFrame = 0;
    return true;
}

uint32_t ApuPlayPosition()
{
    if (!voices)
        return 0;
    volatile uint32_t* offset = reinterpret_cast<volatile uint32_t*>(voices + VOICE * VOICE_SIZE + VOICE_PAR_OFFSET);
    return (*offset & CBO_MASK) % APU_RING_FRAMES;
}

int ApuQueuedFrames()
{
    int play = (int)ApuPlayPosition();
    int queued = (writeFrame - play + APU_RING_FRAMES) % APU_RING_FRAMES;

    // The voice has run past the written frames (the feeder was held up): carry on from where it plays.
    if (queued > APU_RING_FRAMES * 3 / 4) {
        writeFrame = play;
        ++apuCatchUps;
        return 0;
    }
    return queued;
}

int ApuCatchUps()
{
    return apuCatchUps;
}

void ApuWrite(const int16_t* data, int frames)
{
    for (int i = 0; i < frames; ++i) {
        ring[2 * writeFrame] = data[2 * i];
        ring[2 * writeFrame + 1] = data[2 * i + 1];
        writeFrame = (writeFrame + 1) % APU_RING_FRAMES;
    }
}

} // namespace audio
