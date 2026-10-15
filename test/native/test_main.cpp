// Host-side unit tests for the platform-independent firmware code:
// WAV parsing, the power log sector format, the SPSC ring buffer and the mixer (including a two-thread
// producer/consumer test that mirrors core 0 / core 1).
//
// Run: test/run_native_tests.sh [path/to/HDDSynth_Samples]
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "mixer.h"
#include "powerlog_format.h"
#include "ring_buffer.h"
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
    CHECK(wavIsPlayable(info, 16000));
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
    CHECK(wavIsPlayable(info, 16000));
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
        CHECK(!wavIsPlayable(info, 16000));
    }
    {
        WavBuilder w; w.riff(); w.fmt(1, 1, 22050, 16); w.data(16, 16);  // wrong rate
        CHECK(parseWavHeader(w.b.data(), w.b.size(), w.b.size(), info) == WavError::Ok);
        CHECK(!wavIsPlayable(info, 16000));
    }
}

static std::string samplesDir;

TEST(wav_real_sample_packs) {
    if (samplesDir.empty()) {
        std::printf("  (skipped: no sample pack directory given)\n");
        return;
    }
    int files = 0;
    DIR* root = opendir(samplesDir.c_str());
    CHECK(root != nullptr);
    if (!root) return;
    while (dirent* pack = readdir(root)) {
        if (pack->d_name[0] == '.') continue;
        for (const char* name : {"spinup.wav", "idle.wav", "access.wav", "spindown.wav"}) {
            std::string p = samplesDir + "/" + pack->d_name + "/" + name;
            std::ifstream f(p, std::ios::binary | std::ios::ate);
            if (!f) continue;
            const uint32_t size = static_cast<uint32_t>(f.tellg());
            f.seekg(0);
            std::vector<uint8_t> head(1024);
            f.read(reinterpret_cast<char*>(head.data()), head.size());
            WavInfo info;
            const WavError e = parseWavHeader(head.data(), static_cast<size_t>(f.gcount()), size, info);
            const bool ok = e == WavError::Ok && wavIsPlayable(info, 16000) &&
                            info.dataOffset + info.dataBytes <= size;
            if (!ok) std::printf("  FAIL %s: %s\n", p.c_str(), wavErrorName(e));
            CHECK(ok);
            ++files;
        }
    }
    closedir(root);
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

TEST(powerlog_crc32_known_value) {
    const char* s = "123456789";  // the standard CRC-32 check value
    CHECK_EQ(powerlog::crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
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

int main(int argc, char** argv) {
    if (argc > 1) samplesDir = argv[1];
    RUN(wav_basic_pcm);
    RUN(wav_list_chunk_and_odd_padding);
    RUN(wav_chunk_before_fmt);
    RUN(wav_bogus_data_length_clamped);
    RUN(wav_rejects);
    RUN(wav_real_sample_packs);
    RUN(powerlog_crc32_known_value);
    RUN(powerlog_sector_round_trip);
    RUN(powerlog_sector_full_and_clamped);
    RUN(powerlog_sector_rejects_blank_and_torn);
    RUN(ring_wraparound);
    RUN(ring_two_threads);
    RUN(mixer_unity_passthrough_and_end);
    RUN(mixer_memory_loop);
    RUN(mixer_levels_and_saturation);
    RUN(mixer_level_ramp_is_smooth);
    RUN(mixer_stop_request);
    RUN(mixer_restart_rewinds);
    RUN(mixer_stream_underrun_and_eof);
    RUN(mixer_stream_two_threads_no_gaps);
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
