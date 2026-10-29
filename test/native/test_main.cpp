// Host-side unit tests for the platform-independent firmware code:
// WAV parsing, the power log and persisted-state formats, encoder and
// button handling, beep synthesis, OLED screens, the SPSC ring buffer and the mixer (including a two-thread
// producer/consumer test that mirrors core 0 / core 1).
//
// Run: test/run_native_tests.sh [path/to/HDDSynth_Samples]
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

#include "mixer.h"
#include "oled_model.h"
#include "oled_screens.h"
#include "beep.h"
#include "button.h"
#include "canvas.h"
#include "colour.h"
#include "crc32.h"
#include "level_follower.h"
#include "levels_format.h"
#include "host_protocol.h"
#include "link_parser.h"
#include "persist_format.h"
#include "powerlog_format.h"
#include "quadrature.h"
#include "ring_buffer.h"
#include "sentence.h"
#include "wav.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++checks;                                                                \
        auto _a = (a);                                                           \
        auto _b = (b);                                                           \
        if (!(_a == _b)) {                                                       \
            ++failures;                                                          \
            std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, \
                        #a, #b, (long long)_a, (long long)_b);                   \
        }                                                                        \
    } while (0)

#define TEST(name) static void name()
#define RUN(name)                                     \
    do {                                              \
        std::printf("%s\n", #name);                   \
        name();                                       \
    } while (0)

// ---------------------------------------------------------------------------
// WAV builder for synthetic headers
// ---------------------------------------------------------------------------
struct WavBuilder {
    std::vector<uint8_t> b;
    void tag(const char* t) { b.insert(b.end(), t, t + 4); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
    void u16(uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
    void riff() { tag("RIFF"); u32(0); tag("WAVE"); }
    void fmt(uint16_t format, uint16_t ch, uint32_t rate, uint16_t bits) {
        tag("fmt "); u32(16); u16(format); u16(ch); u32(rate); u32(rate * ch * bits / 8);
        u16(ch * bits / 8); u16(bits);
    }
    void chunk(const char* t, uint32_t len) { tag(t); u32(len); for (uint32_t i = 0; i < len + (len & 1); ++i) b.push_back(0); }
    void data(uint32_t len, uint32_t declared) { tag("data"); u32(declared); for (uint32_t i = 0; i < len; ++i) b.push_back(i & 0xFF); }
};

TEST(wav_basic_pcm) {
    WavBuilder w; w.riff(); w.fmt(1, 1, 16000, 16); w.data(1000, 1000);
    WavInfo info;
    CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
    CHECK_EQ(info.dataOffset, 44u);
    CHECK_EQ(info.dataBytes, 1000u);
    CHECK(wavIsPlayable(info));
}

TEST(wav_list_chunk_and_odd_padding) {
    WavBuilder w; w.riff(); w.fmt(1, 1, 16000, 16); w.chunk("LIST", 25); w.data(64, 64);
    WavInfo info;
    CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
    CHECK_EQ(info.dataOffset, 36u + 8 + 26 + 8);  // fmt ends at 36, LIST 8+25+pad, data header 8
    CHECK_EQ(info.dataBytes, 64u);
}

TEST(wav_chunk_before_fmt) {
    WavBuilder w; w.riff(); w.chunk("bext", 600); w.fmt(1, 1, 16000, 16); w.data(32, 32);
    WavInfo info;
    CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
    CHECK(wavIsPlayable(info));
}

TEST(wav_large_metadata_fits_the_4k_header_read) {
    // Editors can put several KB of metadata before the audio. The firmware
    // reads cfg::WAV_HEADER_BYTES from the start of each WAV (sd_streamer).
    WavBuilder w; w.riff(); w.chunk("LIST", 3000); w.fmt(1, 1, 16000, 16); w.data(50000, 50000);
    WavInfo info;
    CHECK(parseWavHeader(w.b.data(), cfg::WAV_HEADER_BYTES, w.b.size(), info) == WavError::Ok);
    CHECK_EQ(info.dataOffset, 12u + 8 + 3000 + 24 + 8);
    CHECK(wavIsPlayable(info));
    // The old 1 KB read couldn't reach the data chunk.
    CHECK(parseWavHeader(w.b.data(), 1024, w.b.size(), info) == WavError::Truncated);
}

TEST(wav_bogus_data_length_clamped) {
    WavBuilder w; w.riff(); w.fmt(1, 1, 16000, 16); w.data(101, 0xFFFFFFFF);
    WavInfo info;
    CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
    CHECK_EQ(info.dataBytes, 100u);  // clamped to file, odd byte dropped (whole frames only)
}

TEST(wav_rejects) {
    WavInfo info;
    {
        WavBuilder w; w.tag("RIFX"); w.u32(0); w.tag("WAVE");
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::NotRiff);
    }
    {
        WavBuilder w; w.riff(); w.fmt(1, 1, 16000, 16);
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::NoData);
    }
    {
        WavBuilder w; w.riff(); w.data(16, 16);
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::NoFmt);
    }
    {
        WavBuilder w; w.riff(); w.fmt(3, 1, 16000, 32); w.data(16, 16);  // IEEE float
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Unsupported);
    }
    {
        WavBuilder w; w.riff(); w.chunk("LIST", 2000); w.fmt(1, 1, 16000, 16); w.data(16, 16);
        // Only the first 512 bytes available: header runs past the buffer.
        CHECK(parseWavHeader(w.b.data(), 512, w.b.size(), info) == WavError::Truncated);
    }
    {
        WavBuilder w; w.riff(); w.fmt(1, 2, 16000, 16); w.data(16, 16);  // stereo
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
        CHECK(!wavIsPlayable(info));
    }
    for (uint32_t rate : {16000u, 22050u, 32000u, 44100u, 48000u}) {  // supported rates
        WavBuilder w; w.riff(); w.fmt(1, 1, rate, 16); w.data(16, 16);
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
        CHECK(wavIsPlayable(info));
    }
    for (uint32_t rate : {8000u, 11025u, 24000u, 96000u}) {  // unsupported rates
        WavBuilder w; w.riff(); w.fmt(1, 1, rate, 16); w.data(16, 16);
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
        CHECK(!wavIsPlayable(info));
    }
    {
        WavBuilder w; w.riff(); w.fmt(1, 1, 22050, 24); w.data(18, 18);  // 24-bit
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
        CHECK(!wavIsPlayable(info));
    }
}

static std::string samplesDir;

TEST(wav_real_sample_packs) {
    if (samplesDir.empty()) {
        std::printf("  (skipped: no sample pack directory given)\n");
        return;
    }
    int files = 0;
    // Skips a file that doesn't exist; one that exists must be playable.
    auto checkWav = [&](const std::string& p) {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return;
        const uint32_t size = static_cast<uint32_t>(f.tellg());
        f.seekg(0);
        std::vector<uint8_t> head(cfg::WAV_HEADER_BYTES);  // as much as the firmware reads
        f.read(reinterpret_cast<char*>(head.data()), head.size());
        WavInfo info;
        const WavError e = parseWavHeader(head.data(), static_cast<size_t>(f.gcount()), size, info);
        const bool ok = e == WavError::Ok && wavIsPlayable(info) && info.dataOffset + info.dataBytes <= size;
        if (!ok) std::printf("  FAIL %s: %s\n", p.c_str(), wavErrorName(e));
        CHECK(ok);
        ++files;
    };
    // HDDSynth_Samples mirrors the SD card: packs in samples/<pack>/, the
    // jingle at the root. (Older checkouts had the packs at the top level.)
    std::string packsDir = samplesDir + "/samples";
    DIR* root = opendir(packsDir.c_str());
    if (!root) {
        packsDir = samplesDir;
        root = opendir(packsDir.c_str());
    }
    CHECK(root != nullptr);
    if (!root) return;
    int packs = 0;
    while (dirent* pack = readdir(root)) {
        if (pack->d_name[0] == '.') continue;
        const int before = files;
        for (const char* name : {"spinup.wav", "idle.wav", "access.wav", "spindown.wav"}) {
            checkWav(packsDir + "/" + pack->d_name + "/" + name);
        }
        if (files > before) ++packs;
    }
    closedir(root);
    checkWav(samplesDir + "/jingle.wav");  // played from the SD card root on first boot
    std::printf("  %d packs found\n", packs);
    CHECK(packs > 0);  // a layout change mustn't silently skip every pack
    std::printf("  %d sample files parsed\n", files);
    CHECK(files > 0);
}

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------
TEST(ring_wraparound) {
    SpscRing<int16_t, 8> r;
    int16_t in[6] = {1, 2, 3, 4, 5, 6}, out[8];
    CHECK_EQ(r.push(in, 6), 6u);
    CHECK_EQ(r.pop(out, 4), 4u);
    CHECK_EQ(r.push(in, 6), 6u);   // wraps
    CHECK_EQ(r.free(), 0u);
    CHECK_EQ(r.push(in, 1), 0u);   // full
    CHECK_EQ(r.pop(out, 8), 8u);
    const int16_t expect[8] = {5, 6, 1, 2, 3, 4, 5, 6};
    CHECK(std::memcmp(out, expect, sizeof(expect)) == 0);
    CHECK_EQ(r.pop(out, 1), 0u);   // empty
}

TEST(ring_two_threads) {
    // Producer and consumer on separate threads, like core 0 / core 1.
    static SpscRing<int16_t, 1024> r;
    constexpr int TOTAL = 4'000'000;
    std::atomic<bool> bad{false};
    std::thread producer([&] {
        int16_t buf[100];
        int next = 0;
        while (next < TOTAL) {
            int n = 0;
            while (n < 100 && next + n < TOTAL) { buf[n] = static_cast<int16_t>(next + n); ++n; }
            size_t pushed = r.push(buf, n);
            next += static_cast<int>(pushed);
        }
    });
    int expect = 0;
    int16_t buf[77];
    while (expect < TOTAL) {
        size_t got = r.pop(buf, 77);
        for (size_t i = 0; i < got; ++i, ++expect)
            if (buf[i] != static_cast<int16_t>(expect)) bad = true;
    }
    producer.join();
    CHECK(!bad);
}

// ---------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------
static void startMem(Voice& v, const int16_t* data, uint32_t len, bool loop, uint16_t level) {
    v.mem = data; v.memLen = len; v.ring = nullptr; v.loop = loop;
    v.targetLevel = level;
    v.playGen.fetch_add(1);
    v.state = VS_PLAYING;
}

static void startRing(Voice& v, SampleRing* ring, uint16_t level) {
    v.mem = nullptr; v.ring = ring; v.loop = false;
    v.targetLevel = level;
    v.playGen.fetch_add(1);
    v.state = VS_PLAYING;
}

TEST(mixer_unity_passthrough_and_end) {
    Voice v[1];
    std::vector<int16_t> src(200);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<int16_t>(i * 100 - 10000);
    startMem(v[0], src.data(), 200, false, LEVEL_UNITY);
    int16_t out[128];
    mixBlock(v, 1, out, 128);
    CHECK(std::memcmp(out, src.data(), 128 * 2) == 0);
    CHECK_EQ(v[0].state.load(), VS_PLAYING);
    mixBlock(v, 1, out, 128);  // 72 samples left, then silence
    CHECK(std::memcmp(out, src.data() + 128, 72 * 2) == 0);
    CHECK_EQ(out[72], 0);
    CHECK_EQ(v[0].state.load(), VS_IDLE);
}

TEST(mixer_memory_loop) {
    Voice v[1];
    const int16_t src[3] = {10, 20, 30};
    startMem(v[0], src, 3, true, LEVEL_UNITY);
    int16_t out[128];
    mixBlock(v, 1, out, 128);
    bool ok = true;
    for (int i = 0; i < 128; ++i) ok &= out[i] == src[i % 3];
    CHECK(ok);
    CHECK_EQ(v[0].state.load(), VS_PLAYING);
}

TEST(mixer_levels_and_saturation) {
    Voice v[2];
    std::vector<int16_t> loud(128, 30000);
    startMem(v[0], loud.data(), 128, true, LEVEL_UNITY / 2);
    int16_t out[128];
    mixBlock(v, 1, out, 128);
    CHECK_EQ(out[10], 15000);
    startMem(v[0], loud.data(), 128, true, LEVEL_UNITY);
    startMem(v[1], loud.data(), 128, true, LEVEL_UNITY);
    mixBlock(v, 2, out, 128);
    CHECK_EQ(out[0], 32767);  // clamped, no wraparound
}

TEST(mixer_gain_up_to_two) {
    CHECK_EQ(levelFromFloat(0.5f), 16384);
    CHECK_EQ(levelFromFloat(1.0f), LEVEL_UNITY);
    CHECK_EQ(levelFromFloat(1.5f), 49152);   // a pack GAIN above 1
    CHECK_EQ(levelFromFloat(2.0f), 65535);   // the most a uint16 Q15 level holds
    CHECK_EQ(levelFromFloat(5.0f), 65535);
    Voice v[1];
    std::vector<int16_t> quiet(128, 10000), loud(128, 30000), low(128, -30000);
    int16_t out[128];
    startMem(v[0], quiet.data(), 128, true, levelFromFloat(2.0f));
    mixBlock(v, 1, out, 128);
    CHECK(out[5] >= 19998 && out[5] <= 20000);  // doubled
    startMem(v[0], loud.data(), 128, true, levelFromFloat(2.0f));
    mixBlock(v, 1, out, 128);
    CHECK_EQ(out[5], 32767);                    // saturates, doesn't wrap
    startMem(v[0], low.data(), 128, true, levelFromFloat(2.0f));
    mixBlock(v, 1, out, 128);
    CHECK_EQ(out[5], -32768);
}

TEST(mixer_level_ramp_is_smooth) {
    Voice v[1];
    std::vector<int16_t> dc(256, 20000);
    startMem(v[0], dc.data(), 256, true, 0);
    int16_t out[128];
    mixBlock(v, 1, out, 128);
    CHECK_EQ(out[127], 0);
    v[0].targetLevel = LEVEL_UNITY;  // access switched on
    mixBlock(v, 1, out, 128);
    int maxStep = 0;
    for (int i = 1; i < 128; ++i) maxStep = std::max(maxStep, std::abs(out[i] - out[i - 1]));
    CHECK(maxStep < 400);            // a hard switch would be a 20000 step
    CHECK(std::abs(out[127] - 20000) <= 1);
}

TEST(mixer_stop_request) {
    Voice v[1];
    const int16_t src[4] = {1, 2, 3, 4};
    startMem(v[0], src, 4, true, LEVEL_UNITY);
    v[0].state = VS_STOP_REQ;
    int16_t out[128];
    mixBlock(v, 1, out, 128);
    CHECK_EQ(v[0].state.load(), VS_IDLE);
    CHECK_EQ(out[0], 0);
}

TEST(mixer_stop_fades_out_over_one_block) {
    Voice v[1];
    std::vector<int16_t> loud(1000, 20000);
    startMem(v[0], loud.data(), 1000, true, LEVEL_UNITY);
    int16_t out[128];
    mixBlock(v, 1, out, 128);  // sounding at full level
    CHECK_EQ(out[127], 20000);
    v[0].state = VS_STOP_REQ;
    mixBlock(v, 1, out, 128);  // the stop: one block ramping down, no cliff
    CHECK(out[0] > 19000);
    bool falling = true;
    for (int i = 1; i < 128; ++i) falling &= out[i] <= out[i - 1];
    CHECK(falling);
    CHECK(out[127] < 200);
    CHECK_EQ(v[0].state.load(), VS_IDLE);
    mixBlock(v, 1, out, 128);  // and silent after
    CHECK_EQ(out[0], 0);
}

TEST(mixer_restart_rewinds) {
    Voice v[1];
    std::vector<int16_t> src(1000);
    for (int i = 0; i < 1000; ++i) src[i] = static_cast<int16_t>(i);
    startMem(v[0], src.data(), 1000, false, LEVEL_UNITY);
    int16_t out[128];
    mixBlock(v, 1, out, 128);
    mixBlock(v, 1, out, 128);
    CHECK_EQ(out[0], 128);
    startMem(v[0], src.data(), 1000, false, LEVEL_UNITY);  // same voice, new start
    mixBlock(v, 1, out, 128);
    CHECK_EQ(out[0], 0);
}

TEST(mixer_stream_underrun_and_eof) {
    static SampleRing ring;
    ring.reset();
    Voice v[1];
    std::vector<int16_t> chunk(100, 1000);
    ring.push(chunk.data(), chunk.size());
    startRing(v[0], &ring, LEVEL_UNITY);
    int16_t out[128];
    mixBlock(v, 1, out, 128);                 // 100 real samples, 28 missing
    CHECK_EQ(v[0].underrunSamples.load(), 28u);
    CHECK_EQ(v[0].state.load(), VS_PLAYING);  // underrun is not the end
    ring.push(chunk.data(), 50);
    v[0].eof = true;                          // producer finished
    mixBlock(v, 1, out, 128);
    CHECK_EQ(out[49], 1000);
    CHECK_EQ(out[50], 0);
    CHECK_EQ(v[0].state.load(), VS_IDLE);
    CHECK_EQ(v[0].underrunSamples.load(), 28u);  // draining at eof is not an underrun
}

TEST(mixer_stream_two_threads_no_gaps) {
    // Producer thread streams a ramp into the ring (core 0); this thread mixes
    // at unity gain (core 1). Output must be the exact ramp: no gaps, no repeats.
    static SampleRing ring;
    ring.reset();
    Voice v[1];
    constexpr int TOTAL = 2'000'000;
    // Prime like sd::prime() does before the voice starts.
    int next = 0;
    {
        std::vector<int16_t> buf(cfg::RING_SAMPLES);
        for (auto& s : buf) s = static_cast<int16_t>(next++);
        ring.push(buf.data(), buf.size());
    }
    startRing(v[0], &ring, LEVEL_UNITY);
    std::thread producer([&] {
        int16_t buf[cfg::SD_CHUNK_SAMPLES];
        while (next < TOTAL) {
            if (ring.free() < cfg::SD_CHUNK_SAMPLES) { std::this_thread::yield(); continue; }
            int n = 0;
            while (n < (int)cfg::SD_CHUNK_SAMPLES && next < TOTAL) buf[n++] = static_cast<int16_t>(next++);
            ring.push(buf, n);
        }
        v[0].eof.store(true, std::memory_order_release);
    });
    int expect = 0;
    bool ok = true;
    int16_t out[cfg::MIX_BLOCK_FRAMES];
    while (v[0].state.load() == VS_PLAYING) {
        if (ring.size() < cfg::MIX_BLOCK_FRAMES && !v[0].eof.load()) { std::this_thread::yield(); continue; }
        mixBlock(v, 1, out, cfg::MIX_BLOCK_FRAMES);
        for (size_t i = 0; i < cfg::MIX_BLOCK_FRAMES && expect < TOTAL; ++i, ++expect)
            ok &= out[i] == static_cast<int16_t>(expect);
    }
    producer.join();
    CHECK(ok);
    CHECK_EQ(expect, TOTAL);
    CHECK_EQ(v[0].underrunSamples.load(), 0u);
}


// ---- power log sector format ------------------------------------------------

TEST(crc32_known_value) {
    const char* s = "123456789";  // the standard CRC-32 check value
    CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST(powerlog_sector_round_trip) {
    uint8_t sector[powerlog::SECTOR_BYTES];
    const char* line = "[trace] spindown t=49104 supercap=4105mV flt=0\n";
    powerlog::encodeSector(sector, 7, 3, line, std::strlen(line));
    uint32_t session = 0;
    uint16_t index = 0;
    const char* text = nullptr;
    CHECK_EQ(powerlog::decodeSector(sector, session, index, text), (int)std::strlen(line));
    CHECK_EQ(session, 7u);
    CHECK_EQ(index, 3);
    CHECK(text && std::memcmp(text, line, std::strlen(line)) == 0);
}

TEST(powerlog_sector_full_and_clamped) {
    uint8_t sector[powerlog::SECTOR_BYTES];
    std::vector<char> big(powerlog::TEXT_BYTES + 100, 'x');
    powerlog::encodeSector(sector, 1, 0, big.data(), big.size());
    uint32_t session;
    uint16_t index;
    const char* text;
    CHECK_EQ(powerlog::decodeSector(sector, session, index, text), (int)powerlog::TEXT_BYTES);
}

TEST(powerlog_sector_rejects_blank_and_torn) {
    uint8_t sector[powerlog::SECTOR_BYTES];
    uint32_t session;
    uint16_t index;
    const char* text;
    std::memset(sector, 0, sizeof(sector));
    CHECK_EQ(powerlog::decodeSector(sector, session, index, text), -1);
    std::memset(sector, 0xFF, sizeof(sector));  // erased flash / unwritten card
    CHECK_EQ(powerlog::decodeSector(sector, session, index, text), -1);

    powerlog::encodeSector(sector, 2, 0, "hello\n", 6);
    sector[powerlog::HEADER_BYTES + 1] ^= 0x01;  // one flipped bit in the text: torn write
    CHECK_EQ(powerlog::decodeSector(sector, session, index, text), -1);

    powerlog::encodeSector(sector, 2, 0, "hello\n", 6);
    sector[10] = 0xFF;  // length beyond the sector
    sector[11] = 0xFF;
    CHECK_EQ(powerlog::decodeSector(sector, session, index, text), -1);
}


// ---- rotary encoder quadrature ----------------------------------------------

// Feed a sequence of A/B states (bit 1 = A, bit 0 = B); return the detent sum.
static int feed(QuadratureDecoder& d, std::initializer_list<uint8_t> states) {
    int total = 0;
    for (uint8_t ab : states) total += d.update(ab);
    return total;
}

TEST(quadrature_one_detent_each_way) {
    QuadratureDecoder d;
    CHECK_EQ(feed(d, {0b01, 0b00, 0b10, 0b11}), 1);   // A leads
    CHECK_EQ(feed(d, {0b10, 0b00, 0b01, 0b11}), -1);  // B leads
    CHECK_EQ(feed(d, {0b01, 0b00, 0b10, 0b11, 0b01, 0b00, 0b10, 0b11, 0b01, 0b00, 0b10, 0b11}), 3);
}

TEST(quadrature_reports_only_at_rest) {
    QuadratureDecoder d;
    CHECK_EQ(d.update(0b01), 0);
    CHECK_EQ(d.update(0b00), 0);
    CHECK_EQ(d.update(0b10), 0);
    CHECK_EQ(d.update(0b11), 1);
}

TEST(quadrature_contact_bounce_cancels) {
    QuadratureDecoder d;
    // Each edge chatters before settling; still exactly one detent.
    CHECK_EQ(feed(d, {0b01, 0b11, 0b01, 0b00, 0b01, 0b00, 0b10, 0b00, 0b10, 0b11, 0b10, 0b11}), 1);
    // Wiggling half-way into a detent and back is not a detent.
    CHECK_EQ(feed(d, {0b01, 0b00, 0b01, 0b11}), 0);
    CHECK_EQ(feed(d, {0b10, 0b11, 0b10, 0b11}), 0);
}

TEST(quadrature_missed_transition_still_counts) {
    QuadratureDecoder d;
    // A fast turn skips 00 (01 -> 10 changes both bits): 2 valid quarters.
    CHECK_EQ(feed(d, {0b01, 0b10, 0b11}), 1);
    CHECK_EQ(feed(d, {0b10, 0b01, 0b11}), -1);
    // Repeated states (interrupt for an edge already seen) change nothing.
    CHECK_EQ(feed(d, {0b11, 0b11, 0b01, 0b01, 0b00, 0b10, 0b10, 0b11}), 1);
}


// ---- buttons ------------------------------------------------------------------

using Ev = Button::Event;

// Hold the raw state from `fromMs` to `toMs` (1 ms steps); return the events seen.
static std::vector<Ev> hold(Button& b, bool pressed, uint32_t fromMs, uint32_t toMs) {
    std::vector<Ev> events;
    for (uint32_t i = 0; i < toMs - fromMs; ++i) {  // counts, so a wrapped range works too
        const Ev e = b.update(pressed, fromMs + i);
        if (e != Ev::None) events.push_back(e);
    }
    return events;
}

TEST(button_short_press_fires_on_release) {
    Button b(20, 1000, 3000);
    auto down = hold(b, true, 0, 300);
    CHECK(down.size() == 1 && down[0] == Ev::Pressed);
    auto up = hold(b, false, 300, 400);
    CHECK(up.size() == 1 && up[0] == Ev::ShortPress);
}

TEST(button_long_press_fires_while_held_and_once) {
    Button b(20, 1000, 3000);
    auto events = hold(b, true, 0, 5000);
    CHECK(events.size() == 2 && events[0] == Ev::Pressed && events[1] == Ev::LongPress);
    auto up = hold(b, false, 5000, 5100);  // no short press after a long one
    CHECK(up.empty());
}

TEST(button_medium_press_does_nothing) {
    Button b(20, 1000, 3000);
    hold(b, true, 0, 2000);  // longer than a short press, shorter than a long one
    CHECK(hold(b, false, 2000, 2100).empty());
}

TEST(button_debounces_bounce_and_glitches) {
    Button b(20, 1000, 3000);
    // A 5 ms glitch is ignored entirely.
    CHECK(hold(b, true, 0, 5).empty());
    CHECK(hold(b, false, 5, 100).empty());
    // A bouncy press (chatter for 10 ms) is one press and one short press.
    std::vector<Ev> events;
    for (uint32_t t = 100; t < 110; ++t) {
        const Ev e = b.update(t % 2, t);
        if (e != Ev::None) events.push_back(e);
    }
    for (Ev e : hold(b, true, 110, 400)) events.push_back(e);
    for (Ev e : hold(b, false, 400, 500)) events.push_back(e);
    CHECK(events.size() == 2 && events[0] == Ev::Pressed && events[1] == Ev::ShortPress);
}

TEST(button_millis_wraparound) {
    Button b(20, 1000, 3000);
    const uint32_t start = 0xFFFFFF00u;  // millis() wraps every 49.7 days
    hold(b, true, start, start + 500);
    auto up = hold(b, false, start + 500, start + 600);
    CHECK(up.size() == 1 && up[0] == Ev::ShortPress);
}

// ---- persisted state format ---------------------------------------------------

TEST(persist_round_trip) {
    PersistedState in;
    std::strcpy(in.desiredPack, "PB_MicroScience612");
    in.jinglePlayed = true;
    uint8_t rec[persistfmt::RECORD_BYTES];
    persistfmt::encode(in, rec);
    PersistedState out;
    CHECK(persistfmt::decode(rec, out));
    CHECK(std::strcmp(out.desiredPack, "PB_MicroScience612") == 0);
    CHECK(out.jinglePlayed);
}

TEST(persist_rejects_blank_and_corrupt) {
    uint8_t rec[persistfmt::RECORD_BYTES];
    PersistedState out;
    std::memset(rec, 0xFF, sizeof(rec));  // erased flash
    CHECK(!persistfmt::decode(rec, out));
    CHECK(out.desiredPack[0] == 0 && !out.jinglePlayed);  // defaults
    std::memset(rec, 0x00, sizeof(rec));
    CHECK(!persistfmt::decode(rec, out));

    PersistedState in;
    std::strcpy(in.desiredPack, "Mark1");
    persistfmt::encode(in, rec);
    rec[10] ^= 0x20;  // one flipped bit in the pack name
    CHECK(!persistfmt::decode(rec, out));
    CHECK(out.desiredPack[0] == 0);
}

TEST(persist_long_name_is_truncated_and_terminated) {
    PersistedState in;
    std::memset(in.desiredPack, 'x', sizeof(in.desiredPack));  // no terminator at all
    uint8_t rec[persistfmt::RECORD_BYTES];
    persistfmt::encode(in, rec);
    PersistedState out;
    CHECK(persistfmt::decode(rec, out));
    CHECK_EQ(std::strlen(out.desiredPack), sizeof(out.desiredPack) - 1);
}


// ---- beep synthesis -----------------------------------------------------------

TEST(beep_timing_matches_python_profiles) {
    // VOLUME_MODE: 2 x 100 ms notes, 50 ms release buffer each, 40 ms gap.
    const BeepPattern p = makeBeep(16000, 1200, 2, 100, BeepWave::Triangle);
    CHECK_EQ(p.heldSamples, 1600u);
    CHECK_EQ(p.periodSamples, 1600u + 800u + 640u);
    CHECK_EQ(p.totalSamples, 2u * (1600u + 800u) + 640u);  // 340 ms
    // FACTORY_RESET: one 1 s note plus its release buffer.
    CHECK_EQ(makeBeep(16000, 1200, 1, 1000, BeepWave::Triangle).totalSamples, 16800u);
    CHECK_EQ(makeBeep(16000, 1200, 0, 100, BeepWave::Sine).totalSamples, 0u);
}

TEST(beep_level_follows_volume_within_limits) {
    CHECK(beepLevelFor(0.0f) == 0.25f);   // never inaudible
    CHECK(beepLevelFor(0.2f) == 0.25f);
    CHECK(beepLevelFor(0.5f) == 0.5f);    // follows the volume
    CHECK(beepLevelFor(1.0f) == 0.8f);    // never louder than CircuitPython's 0.8
}

TEST(beep_lasts_the_same_time_at_any_rate) {
    // Beeps are made at the pack's output rate; their timing must not change.
    const BeepPattern a = makeBeep(16000, 1200, 3, 100, BeepWave::Triangle);
    const BeepPattern b = makeBeep(48000, 1200, 3, 100, BeepWave::Triangle);
    const BeepPattern c = makeBeep(44100, 1200, 3, 100, BeepWave::Triangle);
    CHECK_EQ(b.totalSamples, 3 * a.totalSamples);
    CHECK_EQ(a.totalSamples * 1000 / 16000, c.totalSamples * 1000 / 44100);  // same length in ms
}

TEST(beep_envelope_fades_in_and_out) {
    beepInit();
    const BeepPattern p = makeBeep(16000, 400, 1, 100, BeepWave::Sine);
    int peak = 0, firstMs = 0, lastMs = 0;
    for (uint32_t n = 0; n < p.totalSamples; ++n) {
        const int a = std::abs(beepSample(p, n));
        if (a > peak) peak = a;
        if (n < 16) firstMs = std::max(firstMs, a);                       // first 1 ms
        if (n >= p.heldSamples + p.releaseSamples - 16 && n < p.heldSamples + p.releaseSamples)
            lastMs = std::max(lastMs, a);                                 // last 1 ms of the release
    }
    CHECK(peak > 32000 && peak <= 32767);  // the attack reaches full scale
    CHECK(firstMs < 2000);                 // starts from silence: no click
    CHECK(lastMs < 1000);                  // and fades back to it
    // Silent after the release, and after the end of the pattern.
    CHECK_EQ(beepSample(p, p.heldSamples + p.releaseSamples + 10), 0);
    CHECK_EQ(beepSample(p, p.totalSamples), 0);
}

TEST(beep_led_flashes_once_per_note) {
    const BeepPattern p = makeBeep(16000, 1200, 3, 100, BeepWave::Triangle);  // BALANCE_MODE
    int flashes = 0;
    bool was = false;
    for (uint32_t n = 0; n < p.totalSamples + 100; ++n) {
        const bool on = beepNoteHeld(p, n);
        if (on && !was) ++flashes;
        was = on;
    }
    CHECK_EQ(flashes, 3);
}

TEST(beep_triangle_shape) {
    beepInit();
    const BeepPattern p = makeBeep(16000, 1000, 1, 1000, BeepWave::Triangle);
    // Well into the sustain (envelope 0.8): one 1 kHz cycle is 16 samples,
    // starting at the bottom of the triangle and peaking half way.
    const uint32_t base = 16000 / 2;  // 500 ms, a whole number of cycles in
    CHECK(beepSample(p, base) < -26000);
    CHECK(beepSample(p, base + 8) > 26000);
}

TEST(mixer_plays_a_beep_then_goes_idle) {
    beepInit();
    const BeepPattern p = makeBeep(16000, 1200, 1, 100, BeepWave::Triangle);
    Voice v[1];
    v[0].beep = &p;
    v[0].targetLevel = LEVEL_UNITY;
    v[0].playGen.fetch_add(1);
    v[0].state = VS_PLAYING;
    int16_t out[128];
    bool match = true;
    uint32_t n = 0;
    while (v[0].state.load() == VS_PLAYING && n < 100000) {
        mixBlock(v, 1, out, 128);
        for (int i = 0; i < 128; ++i, ++n) match &= out[i] == beepSample(p, n);
    }
    CHECK(match);
    CHECK_EQ(v[0].state.load(), VS_IDLE);
    CHECK(n >= p.totalSamples && n < p.totalSamples + 128);  // ends within a block
}


// ---- colours ------------------------------------------------------------------

TEST(hue_gives_vivid_colours) {
    uint8_t r, g, b;
    hueToRgb(0, r, g, b);
    CHECK(r == 255 && g == 0 && b == 0);
    hueToRgb(120, r, g, b);
    CHECK(r == 0 && g == 255 && b == 0);
    hueToRgb(240, r, g, b);
    CHECK(r == 0 && g == 0 && b == 255);
    hueToRgb(360 + 60, r, g, b);  // wraps: yellow
    CHECK(r == 255 && g == 255 && b == 0);
    // Every hue: one channel full, one off (fully saturated and bright, never grey).
    bool vivid = true;
    for (uint16_t h = 0; h < 360; ++h) {
        hueToRgb(h, r, g, b);
        const uint8_t hi = std::max({r, g, b}), lo = std::min({r, g, b});
        vivid &= hi == 255 && lo == 0;
    }
    CHECK(vivid);
}


// ---- LED level follower -------------------------------------------------------

static float feedFor(LevelFollower& f, uint16_t peak, int ms) {
    float b = 0;
    for (int t = 0; t < ms; t += 10) b = f.update(peak, 10);
    return b;
}

TEST(level_follower_silence_is_floor) {
    LevelFollower f(15, 180, 3000, 0.05f);
    CHECK(std::fabs(feedFor(f, 0, 500) - 0.05f) < 1e-4f);
    CHECK(std::fabs(feedFor(f, 50, 500) - 0.05f) < 1e-4f);  // below -50 dBFS: not amplified
}

TEST(level_follower_fast_attack_slow_release) {
    LevelFollower f(15, 180, 3000, 0.05f);
    CHECK(feedFor(f, 20000, 40) > 0.9f);   // a beat lights it up within 40 ms
    const float after50 = feedFor(f, 0, 50);
    CHECK(after50 > 0.3f);                 // ...and fades rather than snapping off
    CHECK(feedFor(f, 0, 1000) < 0.1f);     // back near the floor after a second
}

TEST(level_follower_auto_gain_lifts_quiet_audio) {
    LevelFollower f(15, 180, 3000, 0.05f);
    // A quiet recording (peaks ~ -20 dBFS) still reaches full brightness.
    CHECK(feedFor(f, 3000, 500) > 0.95f);
    // Half that level, shortly after, is clearly dimmer: contrast is kept.
    const float half = feedFor(f, 1500, 300);
    CHECK(half > 0.15f && half < 0.5f);
}


// ---- saved volume/balance -----------------------------------------------------

TEST(levels_record_round_trip_and_corruption) {
    uint8_t sec[levels::SECTOR_BYTES];
    levels::Record in;
    in.seq = 41; in.volumePct = 85; in.balancePct = 30;
    levels::encode(in, sec);
    levels::Record out;
    CHECK(levels::decode(sec, out));
    CHECK(out.seq == 41 && out.volumePct == 85 && out.balancePct == 30);
    sec[12] ^= 0x01;  // torn / corrupt
    CHECK(!levels::decode(sec, out));
    std::memset(sec, 0, sizeof(sec));  // blank file
    CHECK(!levels::decode(sec, out));
    in.volumePct = levels::NOT_SAVED;  // cleared by a factory reset
    levels::encode(in, sec);
    CHECK(levels::decode(sec, out) && out.volumePct == levels::NOT_SAVED);
}

TEST(levels_newest_slot_wins) {
    uint8_t a[levels::SECTOR_BYTES], b[levels::SECTOR_BYTES];
    levels::Record ra, rb, out;
    ra.seq = 7; ra.volumePct = 10;
    rb.seq = 8; rb.volumePct = 20;
    levels::encode(ra, a);
    levels::encode(rb, b);
    CHECK(levels::newest(a, b, out) && out.volumePct == 20);
    CHECK(levels::newest(b, a, out) && out.volumePct == 20);
    b[0] ^= 0xFF;  // power died writing slot B: slot A's older value survives
    CHECK(levels::newest(a, b, out) && out.volumePct == 10);
    ra.seq = 0xFFFFFFFFu; rb.seq = 0;  // sequence wraparound: 0 is newer
    levels::encode(ra, a);
    levels::encode(rb, b);
    CHECK(levels::newest(a, b, out) && out.volumePct == 20);
    std::memset(a, 0, sizeof(a));
    std::memset(b, 0, sizeof(b));
    CHECK(!levels::newest(a, b, out));
}

TEST(levels_saver_debounces_two_seconds) {
    levels::Saver sv(2000);
    sv.baseline(50, 50);
    CHECK(!sv.update(50, 50, 0));         // nothing changed
    CHECK(!sv.update(55, 50, 100));       // turned: wait
    CHECK(!sv.update(60, 50, 1500));      // turned again: timer restarts
    CHECK(!sv.update(60, 50, 3400));      // 1.9 s since the last change
    CHECK(sv.update(60, 50, 3500));       // 2 s: save
    sv.saved();
    CHECK(!sv.update(60, 50, 9000));      // saved: nothing more to do
    // Turned and turned back before the debounce: no write needed.
    CHECK(!sv.update(65, 50, 10000));
    CHECK(!sv.update(60, 50, 10500));
    CHECK(!sv.update(60, 50, 13000));
    // A failed write retries after another debounce.
    CHECK(!sv.update(60, 70, 20000));
    CHECK(sv.update(60, 70, 22000));
    sv.failed(22000);
    CHECK(!sv.update(60, 70, 23000));
    CHECK(sv.update(60, 70, 24000));
}


// ---- OLED ---------------------------------------------------------------------

// Set HDDSYNTH_PREVIEW_DIR to write each screen as a PBM image (for previews).
static void preview(const Canvas& c, const char* name) {
    const char* dir = std::getenv("HDDSYNTH_PREVIEW_DIR");
    if (!dir) return;
    std::ofstream f(std::string(dir) + "/" + name + ".pbm");
    f << "P1\n" << Canvas::WIDTH << " " << Canvas::HEIGHT << "\n";
    for (int y = 0; y < Canvas::HEIGHT; ++y) {
        for (int x = 0; x < Canvas::WIDTH; ++x) f << (c.get(x, y) ? "1 " : "0 ");
        f << "\n";
    }
}

static int litPixels(const Canvas& c, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) n += c.get(x, y);
    return n;
}

TEST(canvas_pixels_and_ssd1306_layout) {
    Canvas c;
    c.pixel(0, 0);
    c.pixel(5, 9);   // page 1, bit 1
    c.pixel(200, 3); // clipped
    CHECK(c.get(0, 0) && c.get(5, 9) && !c.get(6, 9));
    CHECK_EQ(c.data()[0], 0x01);
    CHECK_EQ(c.data()[128 + 5], 0x02);
    c.fillRect(-5, -5, 10, 10);  // clipped at the corner
    CHECK(c.get(4, 4) && !c.get(5, 5));
    c.clear();
    CHECK_EQ(litPixels(c, 0, 0, 127, 63), 0);
}

TEST(canvas_text) {
    Canvas c;
    CHECK_EQ(Canvas::textWidth("Volume"), 35);
    const int end = c.text(0, 0, "A");
    CHECK_EQ(end, 6);
    CHECK(c.get(2, 0) && !c.get(5, 0));  // 'A' apex, then the gap column
    c.fillRect(0, 10, 20, 10);
    c.text(0, 11, "A", false);  // dark text on a lit bar
    CHECK(!c.get(2, 11));
}

TEST(oled_history_pushes_left) {
    oled::SampleHistory h;
    for (int i = 1; i <= 130; ++i) h.push(static_cast<uint8_t>(i));
    CHECK_EQ(h.count(), 128);
    CHECK_EQ(h.ago(0), 130);
    CHECK_EQ(h.ago(127), 3);
}

TEST(oled_duty_meter) {
    oled::DutyMeter m(1000);
    uint8_t v = 0;
    // Each call passes the state during the time since the previous call.
    CHECK(!m.update(false, 0, v));
    CHECK(!m.update(false, 250, v));   // off 0-250
    CHECK(!m.update(true, 750, v));    // on 250-750
    CHECK(m.update(false, 1000, v));   // off 750-1000: one period done
    CHECK(v >= 126 && v <= 128);       // half the time active
    CHECK(!m.update(true, 1500, v));
    CHECK(m.update(true, 2000, v));
    CHECK_EQ(v, 255);                  // all on (500 ms at the end counted from 1000)
}

TEST(oled_console_wraps_and_scrolls) {
    oled::TextConsole t;
    for (int i = 0; i < 10; ++i) {
        char line[32];
        std::snprintf(line, sizeof(line), "line %d\r\n", i);
        t.write(line);
    }
    CHECK(std::strcmp(t.row(oled::TextConsole::ROWS - 1), "") == 0);  // after the last newline
    CHECK(std::strcmp(t.row(oled::TextConsole::ROWS - 2), "line 9") == 0);
    t.write("0123456789012345678901234");  // 25 chars: wraps after 21
    CHECK(std::strcmp(t.row(oled::TextConsole::ROWS - 2), "012345678901234567890") == 0);
    CHECK(std::strcmp(t.row(oled::TextConsole::ROWS - 1), "1234") == 0);
}

TEST(oled_overlay_holds_four_seconds) {
    oled::Overlay o(4000);
    using K = oled::Overlay::Kind;
    CHECK(o.current(0) == K::None);
    o.show(K::Volume, 1000);
    CHECK(o.current(4999) == K::Volume);
    o.show(K::Volume, 4000);  // turned again: timer restarts
    CHECK(o.current(7999) == K::Volume);
    CHECK(o.current(8000) == K::None);
    o.show(K::Balance, 9000);
    CHECK(o.current(9001) == K::Balance);
}

TEST(oled_screens_render) {
    Canvas c;
    oled::SampleHistory h;
    // Bursty, like real HDD activity: mostly quiet, with busy spells.
    uint32_t seed = 12345;
    for (int i = 0; i < 200; ++i) {
        seed = seed * 1103515245u + 12345u;
        const bool busy = (i / 9) % 3 == 0;
        h.push(static_cast<uint8_t>(busy ? 120 + (seed >> 16) % 136 : (seed >> 16) % 40));
    }
    oled::drawGraph(c, "PB_MicroScience612", h);
    preview(c, "activity");
    CHECK(c.get(0, 0) && c.get(127, 9));            // title bar lit edge to edge
    CHECK(litPixels(c, 0, oled::BODY_TOP, 127, 63) > 0);
    CHECK(!c.get(0, oled::TITLE_H));                // gap under the title

    oled::drawVolume(c, 65);
    preview(c, "volume_65");
    // 13 thick columns (5..65%), 7 thin: compare the first and last column.
    CHECK(litPixels(c, 0, 60, 10, 63) > litPixels(c, 117, 60, 127, 63) - 4);
    oled::drawVolume(c, 0);
    preview(c, "volume_0");
    oled::drawVolume(c, 100);
    preview(c, "volume_100");

    oled::drawBalance(c, 50);
    preview(c, "balance_50");
    oled::drawBalance(c, 20);
    preview(c, "balance_20");
    const int leftHalf = litPixels(c, 0, 18, 63, 40), rightHalf = litPixels(c, 64, 18, 127, 40);
    CHECK(leftHalf > rightHalf);  // marker towards idle
    oled::drawBalance(c, 100);
    preview(c, "balance_100");

    oled::TextConsole t;
    t.write("[boot] HDD Synth MK5 C++ firmware v0.13.0\n[pack] Mark1\n[pack] spindown in RAM (3744 ms)\n");
    t.write("[boot] spinup\n[run] idle/access looping\n[volume] 65%\n");
    oled::drawConsole(c, t);
    preview(c, "debug");
    CHECK(litPixels(c, 0, 0, 127, 63) > 0);

    oled::SampleHistory sc;
    for (int i = 0; i < 128; ++i) sc.push(static_cast<uint8_t>(i < 20 ? i * 12 : 250 - (i - 20)));
    oled::drawGraph(c, "Supercap 4.12V", sc);
    preview(c, "supercap");
}

// ---- HDDSynthPro activity link ---------------------------------------------

static bool parseLine(const char* line, LinkState& out) { return parseLinkSentence(line, strlen(line), out); }

// Feed a string; returns how many valid sentences it completed.
static int feedAll(LinkParser& p, const char* text) {
    int n = 0;
    for (const char* c = text; *c; ++c) n += p.feed(*c);
    return n;
}

TEST(link_known_answers) {
    LinkState s;
    CHECK(parseLine("$HDA,1,0,I,-*28", s));
    CHECK(!s.busy && s.cls == ActivityClass::Idle && s.op == LinkOp::None);
    CHECK(parseLine("$HDA,1,1,S,R*4C", s));
    CHECK(s.busy && s.cls == ActivityClass::Sequential && s.op == LinkOp::Read);
    CHECK(parseLine("$HDA,1,1,R,W*48", s));
    CHECK(s.busy && s.cls == ActivityClass::Random && s.op == LinkOp::Write);
    CHECK(parseLine("$HDA,1,1,S,W*49", s));
    CHECK(s.busy && s.cls == ActivityClass::Sequential && s.op == LinkOp::Write);
    CHECK(parseLine("$HDA,1,0,R,R*4C", s));
    CHECK(!s.busy && s.cls == ActivityClass::Random && s.op == LinkOp::Read);
}

TEST(link_rejects_bad_sentences) {
    LinkState s;
    s.busy = true;
    CHECK(!parseLine("$HDA,1,0,I,-*29", s));     // wrong checksum
    CHECK(s.busy);                               // and the output is left alone
    CHECK(!parseLine("$HDA,1,0,I,-28", s));      // no '*'
    CHECK(!parseLine("$HDA,1,0,I,-*2", s));      // one checksum digit
    CHECK(!parseLine("$HDA,1,0,I,-*28X", s));    // three
    CHECK(!parseLine("$HDA,1,0,I,-*2G", s));     // not hex
    CHECK(!parseLine("$HDA,2,0,I,-*2B", s));     // version 2 (valid checksum)
    CHECK(!parseLine("$HDB,1,0,I,-*2B", s));     // another sentence (valid checksum)
    CHECK(!parseLine("$GPGGA,1,2*55", s));
    CHECK(!parseLine("$HDA,1,2,I,-*2A", s));     // busy must be 0 or 1
    CHECK(!parseLine("$HDA,1,0,X,-*39", s));     // unknown class
    CHECK(!parseLine("$HDA,1,0,I,X*5D", s));     // unknown op
    CHECK(!parseLine("$HDA,1,0,II,-*61", s));    // two-character field
    CHECK(!parseLine("HDA,1,0,I,-*28", s));      // no '$'
    CHECK(!parseLine("", s));
}

TEST(link_accepts_lower_case_hex_and_extra_fields) {
    LinkState s;
    CHECK(parseLine("$HDA,1,1,S,R*4c", s));
    CHECK(s.busy && s.cls == ActivityClass::Sequential);
    // A later version's extra fields at the end (checksum over all of it).
    CHECK(parseLine("$HDA,1,1,R,W,42,foo*28", s));
    CHECK(s.busy && s.cls == ActivityClass::Random && s.op == LinkOp::Write);
}

TEST(link_parser_assembles_lines) {
    LinkParser p;
    // Split across feeds, CRLF.
    CHECK_EQ(feedAll(p, "$HDA,1,1,"), 0);
    CHECK_EQ(feedAll(p, "S,R*4C\r\n"), 1);
    CHECK(p.state().busy && p.state().cls == ActivityClass::Sequential);
    // Two in one feed, and '\n' without '\r'.
    CHECK_EQ(feedAll(p, "$HDA,1,0,I,-*28\r\n$HDA,1,1,R,W*48\n"), 2);
    CHECK(p.state().busy && p.state().cls == ActivityClass::Random);
    CHECK_EQ(p.valid(), 3u);
    CHECK_EQ(p.rejected(), 0u);
    // Noise before a '$' is dropped without counting; a bad line is counted.
    CHECK_EQ(feedAll(p, "\xff\x7fgarbage\r\n$HDA,1,0,I,-*29\r\n"), 0);
    CHECK_EQ(p.rejected(), 1u);
    CHECK(p.state().busy);  // unchanged by the bad line
    // A '$' mid-line starts over: the cut-off sentence is rejected.
    CHECK_EQ(feedAll(p, "$HDA,1,1,S$HDA,1,0,I,-*28\r\n"), 1);
    CHECK_EQ(p.rejected(), 2u);
    CHECK(!p.state().busy);
}

TEST(link_parser_drops_long_lines) {
    LinkParser p;
    std::string longLine = "$HDA,1,1,S,R";
    while (longLine.size() < 81) longLine += ",x";
    longLine.resize(81);  // 81 characters before the line ending
    CHECK_EQ(feedAll(p, (longLine + "\r\n").c_str()), 0);
    CHECK_EQ(p.rejected(), 1u);
    CHECK_EQ(feedAll(p, "$HDA,1,1,S,R*4C\r\n"), 1);  // and the next one is fine
    // Exactly 80 (with extra fields and a correct checksum) is accepted.
    std::string body = "HDA,1,1,S,R";
    while (body.size() < 76) body += ",y";
    body.resize(76);  // '$' + 76 + "*hh" = 80
    unsigned sum = 0;
    for (char c : body) sum ^= static_cast<unsigned char>(c);
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
    CHECK_EQ(feedAll(p, ("$" + body + tail).c_str()), 1);
}

TEST(link_watch_timeout) {
    LinkWatch w;
    CHECK(!w.present(0));
    CHECK(!w.present(5000));
    w.seen(10000);
    CHECK(w.present(10000));
    CHECK(w.present(10999));
    CHECK(!w.present(11000));  // lost after 1 s
    w.seen(12000);
    CHECK(w.present(12500));   // and back
    w.seen(0xFFFFFF00u);       // millis() wrapping round
    CHECK(w.present(0x100u));
}

TEST(link_access_sample_choice) {
    {   // Without a link the class is always Random: access.wav, as before.
        AccessChooser c;
        CHECK(c.choose(ActivityClass::Random, true) == AccessSample::Access);
        CHECK(!c.sequential());
    }
    {   // Sequential plays sequential.wav when the pack has one...
        AccessChooser c;
        CHECK(c.choose(ActivityClass::Sequential, true) == AccessSample::Sequential);
        CHECK(c.sequential());
        CHECK(c.choose(ActivityClass::Random, true) == AccessSample::Access);
    }
    {   // ...and access.wav when it doesn't (the LED still knows it's sequential).
        AccessChooser c;
        CHECK(c.choose(ActivityClass::Sequential, false) == AccessSample::Access);
        CHECK(c.sequential());
    }
    {   // Idle doesn't change the choice: the last busy class sticks.
        AccessChooser c;
        CHECK(c.choose(ActivityClass::Idle, true) == AccessSample::Access);
        c.choose(ActivityClass::Sequential, true);
        CHECK(c.choose(ActivityClass::Idle, true) == AccessSample::Sequential);
        c.choose(ActivityClass::Random, true);
        CHECK(c.choose(ActivityClass::Idle, true) == AccessSample::Access);
    }
}

// ---- Sentences and the companion app protocol --------------------------------

static HostParse parseCmd(const char* line, HostCommand& c) { return parseHostCommand(line, strlen(line), c); }

TEST(sentence_format_and_checksum) {
    char buf[64];
    CHECK_EQ(formatSentence(buf, sizeof(buf), "HDA,1,1,S,R"), 17u);
    CHECK(strcmp(buf, "$HDA,1,1,S,R*4C\r\n") == 0);
    CHECK_EQ(formatSentence(buf, sizeof(buf), "HDS,1,0.19.0,RUN,0,8,50,50,4150"), 37u);
    CHECK(strcmp(buf, "$HDS,1,0.19.0,RUN,0,8,50,50,4150*27\r\n") == 0);
    CHECK_EQ(formatSentence(buf, 17, "HDA,1,1,S,R"), 0u);  // one byte short of room for the NUL
    CHECK_EQ(formatSentence(buf, 18, "HDA,1,1,S,R"), 17u);
    const char* body;
    size_t n;
    CHECK(sentenceBody("$HDA,1,0,I,-*28", 15, body, n));
    CHECK_EQ(n, 11u);
    CHECK(!sentenceBody("$HDA,1,0,I,-*27", 15, body, n));
}

TEST(sentence_fields) {
    Field f[3];
    const char* b = "HDC,1,VOL,55";
    CHECK_EQ(splitFields(b, strlen(b), f, 3), 4u);  // counted past `max`
    CHECK(f[0].is("HDC") && f[1].is("1") && f[2].is("VOL"));
    CHECK(!f[2].is("VO") && !f[2].is("VOLUME"));
    Field e[4];
    CHECK_EQ(splitFields("A,,B,", 5, e, 4), 4u);  // empty fields count
    CHECK_EQ(e[1].n, 0u);
    CHECK_EQ(e[3].n, 0u);
    uint32_t v = 0;
    Field num{"0042", 4};
    CHECK(num.toUint(v) && v == 42);
    Field bad{"4x", 2}, empty{"", 0}, neg{"-1", 2};
    CHECK(!bad.toUint(v) && !empty.toUint(v) && !neg.toUint(v));
}

TEST(sentence_line_assembler) {
    LineAssembler a;
    std::vector<std::string> lines;
    int dropped = 0;
    auto feed = [&](const char* t) {
        for (const char* c = t; *c; ++c) {
            const LineAssembler::Result r = a.feed(*c);
            if (r == LineAssembler::Result::Line) lines.push_back(a.line());
            if (r == LineAssembler::Result::Dropped) ++dropped;
        }
    };
    // A '-' inside a sentence is part of the line: the serial console must
    // not read it as "volume down" (handleSerial checks inLine()).
    feed("h$HDA,1,0,I,-*28\r\n");
    CHECK_EQ(lines.size(), 1u);
    CHECK(lines[0] == "$HDA,1,0,I,-*28");
    CHECK(!a.inLine());
    feed("$HDQ,1,");
    CHECK(a.inLine());
    feed("STATUS*78\n");
    CHECK(lines.size() == 2 && lines[1] == "$HDQ,1,STATUS*78");
    feed("$HDQ,1$HDQ,1,PACKS*26\n");  // cut off by a '$'
    CHECK_EQ(dropped, 1);
    CHECK(lines.size() == 3 && lines[2] == "$HDQ,1,PACKS*26");
    std::string longLine(81, 'x');
    feed(("$" + longLine + "\n").c_str());
    CHECK_EQ(dropped, 2);
    CHECK_EQ(lines.size(), 3u);
}

TEST(host_commands) {
    HostCommand c;
    CHECK(parseCmd("$HDQ,1,STATUS*78", c) == HostParse::Command && c.type == HostCommandType::Status);
    CHECK(parseCmd("$HDQ,1,PACKS*26", c) == HostParse::Command && c.type == HostCommandType::Packs);
    CHECK(parseCmd("$HDC,1,VOL,55*07", c) == HostParse::Command && c.type == HostCommandType::Volume && c.value == 55);
    CHECK(parseCmd("$HDC,1,BAL,0*2D", c) == HostParse::Command && c.type == HostCommandType::Balance && c.value == 0);
    CHECK(parseCmd("$HDC,1,BAL,100*2C", c) == HostParse::Command && c.value == 100);
    CHECK(parseCmd("$HDC,1,PACK,3*78", c) == HostParse::Command && c.type == HostCommandType::Pack && c.value == 3);
    CHECK(strcmp(hostCommandName(HostCommandType::Pack), "PACK") == 0);
}

TEST(host_commands_rejected) {
    HostCommand c;
    CHECK(parseCmd("$HDA,1,0,I,-*28", c) == HostParse::NotCommand);  // activity goes to the link parser
    CHECK(parseCmd("$HDQ,1,STATUS*79", c) == HostParse::Invalid);   // checksum
    CHECK(parseCmd("$HDC,1,VOL,101*37", c) == HostParse::Invalid);  // over 100%
    CHECK(parseCmd("$HDC,1,VOL,*07", c) == HostParse::Invalid);     // no value
    CHECK(parseCmd("$HDC,1,VOL,5x*4A", c) == HostParse::Invalid);
    CHECK(parseCmd("$HDQ,1,FOO*2A", c) == HostParse::Invalid);      // unknown query
    CHECK(parseCmd("$HDQ,2,STATUS*7B", c) == HostParse::Invalid);   // version 2
    CHECK(parseCmd("$HDC,1,LED,3*2C", c) == HostParse::Invalid);    // unknown command
    CHECK(parseCmd("$HDQ,1*40", c) == HostParse::Invalid);          // too few fields
    CHECK(parseCmd("HDQ,1,STATUS*78", c) == HostParse::Invalid);    // no '$'
}

TEST(host_sanitise_name) {
    char out[16];
    sanitiseName("IBM, 36LZX*$", out, sizeof(out));
    CHECK(strcmp(out, "IBM_ 36LZX__") == 0);
    sanitiseName("tab\there", out, sizeof(out));
    CHECK(strcmp(out, "tab_here") == 0);
    sanitiseName("a much longer name than fits", out, 8);
    CHECK(strcmp(out, "a much ") == 0);  // truncated, still terminated
}

int main(int argc, char** argv) {
    if (argc > 1) samplesDir = argv[1];
    RUN(wav_basic_pcm);
    RUN(wav_list_chunk_and_odd_padding);
    RUN(wav_chunk_before_fmt);
    RUN(wav_large_metadata_fits_the_4k_header_read);
    RUN(wav_bogus_data_length_clamped);
    RUN(wav_rejects);
    RUN(wav_real_sample_packs);
    RUN(crc32_known_value);
    RUN(powerlog_sector_round_trip);
    RUN(powerlog_sector_full_and_clamped);
    RUN(powerlog_sector_rejects_blank_and_torn);
    RUN(quadrature_one_detent_each_way);
    RUN(quadrature_reports_only_at_rest);
    RUN(quadrature_contact_bounce_cancels);
    RUN(quadrature_missed_transition_still_counts);
    RUN(button_short_press_fires_on_release);
    RUN(button_long_press_fires_while_held_and_once);
    RUN(button_medium_press_does_nothing);
    RUN(button_debounces_bounce_and_glitches);
    RUN(button_millis_wraparound);
    RUN(persist_round_trip);
    RUN(persist_rejects_blank_and_corrupt);
    RUN(persist_long_name_is_truncated_and_terminated);
    RUN(beep_timing_matches_python_profiles);
    RUN(beep_level_follows_volume_within_limits);
    RUN(beep_lasts_the_same_time_at_any_rate);
    RUN(beep_envelope_fades_in_and_out);
    RUN(beep_led_flashes_once_per_note);
    RUN(beep_triangle_shape);
    RUN(mixer_plays_a_beep_then_goes_idle);
    RUN(hue_gives_vivid_colours);
    RUN(level_follower_silence_is_floor);
    RUN(level_follower_fast_attack_slow_release);
    RUN(level_follower_auto_gain_lifts_quiet_audio);
    RUN(levels_record_round_trip_and_corruption);
    RUN(levels_newest_slot_wins);
    RUN(levels_saver_debounces_two_seconds);
    RUN(canvas_pixels_and_ssd1306_layout);
    RUN(canvas_text);
    RUN(oled_history_pushes_left);
    RUN(oled_duty_meter);
    RUN(oled_console_wraps_and_scrolls);
    RUN(oled_overlay_holds_four_seconds);
    RUN(oled_screens_render);
    RUN(ring_wraparound);
    RUN(ring_two_threads);
    RUN(mixer_unity_passthrough_and_end);
    RUN(mixer_memory_loop);
    RUN(mixer_levels_and_saturation);
    RUN(mixer_gain_up_to_two);
    RUN(mixer_level_ramp_is_smooth);
    RUN(mixer_stop_request);
    RUN(mixer_stop_fades_out_over_one_block);
    RUN(mixer_restart_rewinds);
    RUN(mixer_stream_underrun_and_eof);
    RUN(mixer_stream_two_threads_no_gaps);
    RUN(link_known_answers);
    RUN(link_rejects_bad_sentences);
    RUN(link_accepts_lower_case_hex_and_extra_fields);
    RUN(link_parser_assembles_lines);
    RUN(link_parser_drops_long_lines);
    RUN(link_watch_timeout);
    RUN(link_access_sample_choice);
    RUN(sentence_format_and_checksum);
    RUN(sentence_fields);
    RUN(sentence_line_assembler);
    RUN(host_commands);
    RUN(host_commands_rejected);
    RUN(host_sanitise_name);
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
