// The audio device on the Xbox: plays the mixer's output through the AC'97 at 48 kHz stereo S16, driving the
// controller's PCM-out (and the S/PDIF out in step) bus masters itself. A time-critical feeder thread keeps
// DEVICE_BUFFERS buffers queued (about 64 ms), so a late wake-up does not starve the hardware, and counts free
// buffers from the controller's own position (its current descriptor index), polling every 4 ms: no interrupts.
// In xemu, whose AC'97 output never reaches the host, it plays through the APU instead (audio_apu.cpp). The only
// file of the audio folder that is platform specific, with audio_apu.cpp.
//
// Not nxdk's XAudio: its descriptor lists are a static array the controller reads by physical address as one
// block, so when the linker happens to place it across a page boundary the controller reads garbage descriptors
// halfway through the list and stops for good. 2026-11-11: a build stalled at descriptor 13 on every boot (current
// index 13, 8 samples left, running, not halted), while the same code with one unused function dropped (a
// different layout) played; a list crossing a page is the likely cause. Its interrupt bookkeeping is also not
// atomic against XAudioProvideSamples.
//
// The cold reset may leave the codec's master and PCM outputs muted (their reset value) or keep the kernel's
// settings, depending on the boot; so the device unmutes them itself once the codec is ready again.
#include "audio/mixer.h"

#include "audio/apu.h"

#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdio.h>

namespace audio {

// Device problems logged to the serial port (the first few of each).
constexpr int MAX_REPORTS = 8;

// --- The codec

// The AC'97 controller's memory space: the primary codec's registers at 0x000 (16 bits each), global control and
// status, and the codec access semaphore (reading it claims the AC-link; bit 0 set: busy).
static volatile uint8_t* const AC97 = reinterpret_cast<volatile uint8_t*>(0xFEC00000u);
constexpr uint32_t GLOB_CNT = 0x12C;
constexpr uint32_t GLOB_STA = 0x130;
constexpr uint32_t CODEC_SEMAPHORE = 0x134;
constexpr uint32_t CODEC_READY = 0x100;

// The two codec registers the device writes: master at 0 dB and PCM out at 0 dB (0x0808), the same writes the
// kernel's boot sound makes.
constexpr uint8_t CODEC_MASTER = 0x02;
constexpr uint8_t CODEC_PCM = 0x18;
constexpr uint16_t MASTER_ON = 0x0000;
constexpr uint16_t PCM_ON = 0x0808;

static uint32_t Reg32(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t*>(AC97 + offset);
}

// Waits (up to 1 ms) for the AC-link to be free and claims it.
static void ClaimCodec()
{
    for (int i = 0; i < 1000; ++i) {
        if ((AC97[CODEC_SEMAPHORE] & 1) == 0)
            return;
        KeStallExecutionProcessor(1);
    }
}

static void CodecWrite(uint8_t reg, uint16_t value)
{
    ClaimCodec();
    *reinterpret_cast<volatile uint16_t*>(AC97 + reg) = value;
}

static void Sleep(int ms)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// After the cold reset: waits for the codec to come back, then unmutes master and PCM out (their reset values may be
// muted). Written twice, 10 ms apart, as the codec may still be settling. Codec reads return 0 on this
// controller, so the writes cannot be checked.
static void SetUpCodec()
{
    for (int ms = 0; ms < 500 && (Reg32(GLOB_STA) & CODEC_READY) == 0; ++ms)
        Sleep(1);
    for (int pass = 0; pass < 2; ++pass) {
        CodecWrite(CODEC_MASTER, MASTER_ON);
        CodecWrite(CODEC_PCM, PCM_ON);
        Sleep(10);
    }
    DbgPrint("RedBaboon audio: codec unmuted (global status %08x)\n", Reg32(GLOB_STA));
}

// --- Feeding the AC'97

// Buffers queued in the AC'97 and frames in each: the latency is at most their product (64 ms).
constexpr int DEVICE_BUFFERS = 6;
constexpr int DEVICE_FRAMES = 512;

// Descriptors in each bus master's ring, used in order from 0 after the reset.
constexpr int DESCRIPTORS = 32;

// The PCM-out and S/PDIF-out bus masters (each: descriptor list base, current index, last valid index, status,
// position, control) and their bits: control "run" and "reset registers"; status "DMA controller halted".
constexpr uint32_t PCM_OUT = 0x110;
constexpr uint32_t SPDIF_OUT = 0x170;
constexpr uint32_t BDBAR = 0x0;
constexpr uint32_t CIV = 0x4;
constexpr uint32_t LVI = 0x5;
constexpr uint32_t SR = 0x6;
constexpr uint32_t CR = 0xB;
constexpr uint8_t CR_RUN = 0x01;
constexpr uint8_t CR_RESET = 0x02;
constexpr uint8_t STATUS_HALTED = 0x01;
constexpr uint16_t STATUS_CLEAR = 0x1C;
constexpr uint32_t GLOB_CNT_COLD_RESET_OFF = 0x02;

// A buffer descriptor: physical address, length in 16-bit samples, flags (none: the device polls).
struct Descriptor {
    uint32_t address;
    uint16_t samples;
    uint16_t flags;
};

// The two descriptor lists (one page-aligned block each, so the controller reads them whole).
static Descriptor* pcmDescriptors = nullptr;
static Descriptor* spdifDescriptors = nullptr;

// Longest sleep of the feeder between checks (100 ns units, negative = relative): 4 ms, under half a buffer.
constexpr LONGLONG POLL_INTERVAL = -40000;

// The buffers (contiguous memory the controller reads, used in turn), the next buffer to fill, and the descriptor
// it goes into.
static int16_t* buffers[DEVICE_BUFFERS];
static int nextBuffer = 0;
static int nextDescriptor = 0;

static uint8_t Reg8(uint32_t offset)
{
    return AC97[offset];
}

static void Set8(uint32_t offset, uint8_t value)
{
    AC97[offset] = value;
}

static void Set16(uint32_t offset, uint16_t value)
{
    *reinterpret_cast<volatile uint16_t*>(AC97 + offset) = value;
}

static void Set32(uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t*>(AC97 + offset) = value;
}

// Hands a buffer to both bus masters as the next descriptor and makes it the last valid one.
static void Provide(const int16_t* buffer)
{
    uint32_t address = (uint32_t)MmGetPhysicalAddress(const_cast<int16_t*>(buffer));
    uint16_t samples = (uint16_t)(DEVICE_FRAMES * 2);
    pcmDescriptors[nextDescriptor] = Descriptor{address, samples, 0};
    spdifDescriptors[nextDescriptor] = Descriptor{address, samples, 0};
    Set8(PCM_OUT + LVI, (uint8_t)nextDescriptor);
    Set8(SPDIF_OUT + LVI, (uint8_t)nextDescriptor);
    nextDescriptor = (nextDescriptor + 1) % DESCRIPTORS;
}

// Starts (or restarts) both bus masters.
static void Run()
{
    Set8(PCM_OUT + CR, CR_RUN);
    Set8(SPDIF_OUT + CR, CR_RUN);
}

// Resets a bus master's registers (stops it; its current index goes back to 0) and points it at its list.
static void ResetChannel(uint32_t channel, const Descriptor* list)
{
    Set8(channel + CR, 0);
    Set8(channel + CR, CR_RESET);
    for (int i = 0; i < 1000 && (Reg8(channel + CR) & CR_RESET) != 0; ++i)
        KeStallExecutionProcessor(1);
    Set16(channel + SR, STATUS_CLEAR);
    Set32(channel + BDBAR, (uint32_t)MmGetPhysicalAddress(const_cast<Descriptor*>(list)));
}

// Cold-resets the codec (it comes back with its registers at their reset values) and resets both bus masters.
static void ResetController()
{
    Set32(GLOB_CNT, Reg32(GLOB_CNT) & ~GLOB_CNT_COLD_RESET_OFF);
    KeStallExecutionProcessor(10);
    Set32(GLOB_CNT, Reg32(GLOB_CNT) | GLOB_CNT_COLD_RESET_OFF);
    ResetChannel(PCM_OUT, pcmDescriptors);
    ResetChannel(SPDIF_OUT, spdifDescriptors);
}

// Buffers handed to the controller and not yet played, counting the one playing now. More than DEVICE_BUFFERS
// means the controller has run past the last buffer it was given.
static int BuffersQueued()
{
    int playing = Reg8(PCM_OUT + CIV) % DESCRIPTORS;
    return (nextDescriptor - playing + DESCRIPTORS) % DESCRIPTORS;
}

// A buffer of silence, for descriptors skipped when the controller has to be caught up.
static int16_t* silence = nullptr;

// How many times the feeder loop has run, and had to restart or catch up the controller.
static volatile uint32_t feederLoops = 0;
static int restarts = 0;
static int catchUps = 0;

// Mixes the next buffer and queues it. Its memory last held the buffer queued DEVICE_BUFFERS turns ago, which has
// played: at most DEVICE_BUFFERS - 1 others are queued when this is called.
static void QueueBuffer()
{
    int16_t* buffer = buffers[nextBuffer];
    nextBuffer = (nextBuffer + 1) % DEVICE_BUFFERS;
    MixerRender(buffer, DEVICE_FRAMES);
    Provide(buffer);
}

// The controller ran past our last buffer (into old descriptors): hand it silence up to where it is, so our
// descriptor count matches its position again.
static void CatchUp()
{
    while (BuffersQueued() > DEVICE_BUFFERS)
        Provide(silence);
    if (++catchUps <= MAX_REPORTS)
        DbgPrint("RedBaboon audio: AC'97 ran past the queue, caught up (%d)\n", catchUps);
}

// The feeder thread: every 4 ms, tops the queue up to DEVICE_BUFFERS, never more, and restarts the controller if it
// stopped (it halts when it runs out of buffers, after the feeder was held up).
static DWORD WINAPI Feeder(LPVOID parameter)
{
    (void)parameter;
    LARGE_INTEGER interval;
    interval.QuadPart = POLL_INTERVAL;
    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        feederLoops = feederLoops + 1;
        if (BuffersQueued() > DEVICE_BUFFERS)
            CatchUp();
        while (BuffersQueued() < DEVICE_BUFFERS)
            QueueBuffer();
        if ((Reg8(PCM_OUT + SR) & STATUS_HALTED) != 0) {
            Run();
            if (++restarts <= MAX_REPORTS)
                DbgPrint("RedBaboon audio: AC'97 had halted (ran dry), restarted (%d)\n", restarts);
        }
    }
    return 0;
}

// --- Output through the APU (xemu)

// Frames the APU feeder keeps written ahead of the voice (43 ms), and the block it mixes at a time.
constexpr int APU_TARGET_FRAMES = 2048;
constexpr int APU_BLOCK_FRAMES = 512;

// True when the game runs in xemu and plays through the APU voice instead of the AC'97.
static bool useApu = false;

// One mixed block for the APU ring.
static int16_t apuBlock[APU_BLOCK_FRAMES * 2];

// True in xemu: its AC'97 model does not keep the global control register (the console reads back the cold-reset
// bit ResetController set), and its AC'97 output never reaches the host, while its APU output does.
static bool RunningInXemu()
{
    return (Reg32(GLOB_CNT) & 2) == 0;
}

// The APU feeder thread: every 4 ms, tops the voice's ring up to APU_TARGET_FRAMES ahead of its play position.
static DWORD WINAPI ApuFeeder(LPVOID parameter)
{
    (void)parameter;
    LARGE_INTEGER interval;
    interval.QuadPart = POLL_INTERVAL;
    for (;;) {
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        feederLoops = feederLoops + 1;
        while (ApuQueuedFrames() < APU_TARGET_FRAMES) {
            MixerRender(apuBlock, APU_BLOCK_FRAMES);
            ApuWrite(apuBlock, APU_BLOCK_FRAMES);
        }
    }
    return 0;
}

// --- Diagnostics

void DeviceLog(const char* text)
{
    DbgPrint("%s\n", text);
}

void DeviceStatus(char* text, int size)
{
    volatile uint8_t* base = reinterpret_cast<volatile uint8_t*>(0xFEC00110u);
    uint32_t civ = base[4];
    uint32_t lvi = base[5];
    uint32_t sr = *reinterpret_cast<volatile uint16_t*>(base + 6);
    uint32_t picb = *reinterpret_cast<volatile uint16_t*>(base + 8);
    uint32_t cr = base[0xB];
    if (useApu) {
        snprintf(text, (size_t)size, "apu voice play %u feeder %u catch-ups %d | glob %08x/%08x", ApuPlayPosition(),
                 feederLoops, ApuCatchUps(), Reg32(GLOB_CNT), Reg32(GLOB_STA));
        return;
    }
    snprintf(text, (size_t)size, "ac97 civ %u lvi %u sr %04x picb %u cr %02x queued %d feeder %u restarts %d "
             "catch-ups %d | glob %08x/%08x",
             civ, lvi, sr, picb, cr, BuffersQueued(), feederLoops, restarts, catchUps, Reg32(GLOB_CNT),
             Reg32(GLOB_STA));
}

// --- Background work

// The background thread (the sound bank build) and what it runs.
static HANDLE backgroundThread = nullptr;
static void (*backgroundWork)() = nullptr;

static DWORD WINAPI Background(LPVOID parameter)
{
    (void)parameter;
    backgroundWork();
    return 0;
}

void DeviceRunInBackground(void (*work)())
{
    backgroundWork = work;
    backgroundThread = CreateThread(nullptr, 0, Background, nullptr, 0, nullptr);
    if (!backgroundThread)
        work();
}

void DeviceWaitForBackground()
{
    if (!backgroundThread)
        return;
    WaitForSingleObject(backgroundThread, INFINITE);
    CloseHandle(backgroundThread);
    backgroundThread = nullptr;
}

// --- Opening the device

bool DeviceOpen()
{
    for (int16_t*& buffer : buffers) {
        buffer = static_cast<int16_t*>(MmAllocateContiguousMemoryEx(DEVICE_FRAMES * 2 * sizeof(int16_t), 0,
                                                                    0xFFFFFFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (!buffer)
            return false;
    }
    silence = static_cast<int16_t*>(MmAllocateContiguousMemoryEx(DEVICE_FRAMES * 2 * sizeof(int16_t), 0, 0xFFFFFFFF,
                                                                 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
    pcmDescriptors = static_cast<Descriptor*>(MmAllocateContiguousMemoryEx(DESCRIPTORS * sizeof(Descriptor), 0,
                                                                           0xFFFFFFFF, 4096, PAGE_READWRITE));
    spdifDescriptors = static_cast<Descriptor*>(MmAllocateContiguousMemoryEx(DESCRIPTORS * sizeof(Descriptor), 0,
                                                                             0xFFFFFFFF, 4096, PAGE_READWRITE));
    if (!silence || !pcmDescriptors || !spdifDescriptors)
        return false;
    for (int i = 0; i < DEVICE_FRAMES * 2; ++i)
        silence[i] = 0;
    for (int i = 0; i < DESCRIPTORS; ++i) {
        pcmDescriptors[i] = Descriptor{0, 0, 0};
        spdifDescriptors[i] = Descriptor{0, 0, 0};
    }
    ResetController();
    SetUpCodec();

    // xemu: play through the APU voice (its AC'97 output is never heard). Needs xemu's "use DSP" setting off.
    if (RunningInXemu() && ApuOpen()) {
        useApu = true;
        DbgPrint("RedBaboon audio: xemu detected, output through the APU voice (xemu: Use DSP must be off)\n");
        HANDLE apuThread = CreateThread(nullptr, 0, ApuFeeder, nullptr, 0, nullptr);
        if (!apuThread)
            return false;
        SetThreadPriority(apuThread, THREAD_PRIORITY_TIME_CRITICAL);
        return true;
    }
    DbgPrint("RedBaboon audio: output through the AC'97\n");
    for (int i = 0; i < DEVICE_BUFFERS; ++i)
        QueueBuffer();
    Run();
    HANDLE thread = CreateThread(nullptr, 0, Feeder, nullptr, 0, nullptr);
    if (!thread)
        return false;
    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    return true;
}

} // namespace audio
