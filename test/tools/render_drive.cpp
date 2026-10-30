// Renders the modelled drive (src/drive_engine.*) on the host, to listen to it
// and to compare it with the browser prototype (tools/drive-lab/test.js runs
// the same scenario on the prototype's engine and prints the same figures).
//
//   render_drive [preset] [out.wav] [sample rate]
//
// The scenario: power on and wait for Ready, 1 s idle, a full-stroke and a
// third-of-a-stroke seek, 4 s each of random, file copy, DOS boot and defrag,
// 2 s at emulator pace, then power off and coast to a stop. Built and run by
// test/run_native_tests.sh (which checks it against the prototype's figures).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "drive_engine.h"

namespace {

struct Acc {
    double sq = 0;
    size_t n = 0;
    float peak = 0;
};

uint32_t rate = 48000;
DriveEngine engine;
std::vector<int16_t> wav;
bool keepWav = false;
bool failed = false;

void run(float secs, Acc& a) {
    const size_t blocks = static_cast<size_t>(secs * rate / 128 + 0.5f);
    float buf[128];
    for (size_t b = 0; b < blocks; ++b) {
        engine.renderFloat(buf, 128);
        for (float y : buf) {
            if (!std::isfinite(y)) failed = true;
            a.sq += double(y) * y;
            a.n++;
            if (std::fabs(y) > a.peak) a.peak = std::fabs(y);
            if (keepWav) wav.push_back(static_cast<int16_t>(y * 32767.0f));
        }
    }
}

void line(const char* what, const Acc& a, const char* extra = "") {
    std::printf("%-7s rms %.3f peak %.2f%s\n", what, std::sqrt(a.sq / (a.n ? a.n : 1)), a.peak, extra);
}

void writeWav(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); return; }
    const uint32_t data = static_cast<uint32_t>(wav.size() * 2), riff = 36 + data, fmtLen = 16, byteRate = rate * 2;
    const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f); std::fwrite(wav.data(), 2, wav.size(), f);
    std::fclose(f);
    std::printf("wrote %s (%.1f s)\n", path, wav.size() / double(rate));
}

} // namespace

int main(int argc, char** argv) {
    const char* preset = argc > 1 ? argv[1] : "voice97";
    const char* out = argc > 2 ? argv[2] : nullptr;
    if (argc > 3) rate = static_cast<uint32_t>(std::atoi(argv[3]));
    keepWav = out != nullptr;
    DriveParams p;
    if (!drivePreset(preset, p)) { std::fprintf(stderr, "unknown preset %s\n", preset); return 2; }
    engine.begin(rate, p);
    char extra[96];

    Acc a;
    engine.power(true);
    float secs = 0;
    while (engine.state() != DriveState::Ready && secs < 30) { run(0.1f, a); secs += 0.1f; }
    std::snprintf(extra, sizeof(extra), "  ready after %.1f s", secs);
    line("spinup", a, extra);
    a = Acc{}; run(1, a); line("idle", a);
    engine.seekTo(0); run(0.6f, a);
    engine.seekTo(1); a = Acc{}; run(2.5f, a);
    std::snprintf(extra, sizeof(extra), "  full stroke %.1f ms", engine.lastSeekUs() / 1000.0);
    line("stroke", a, extra);
    engine.seekTo(0.66f); run(1.5f, a);
    std::printf("third   %.1f ms\n", engine.lastSeekUs() / 1000.0);
    const DriveWorkload loads[] = {DriveWorkload::Random, DriveWorkload::Sequential, DriveWorkload::Boot, DriveWorkload::Defrag};
    const char* names[] = {"random", "seq", "boot", "defrag"};
    for (int i = 0; i < 4; ++i) {
        engine.setWorkload(loads[i]);
        const uint32_t s0 = engine.seekCount();
        a = Acc{}; run(4, a);
        std::snprintf(extra, sizeof(extra), "  seeks/s %.1f", (engine.seekCount() - s0) / 4.0);
        line(names[i], a, extra);
    }
    engine.setWorkload(DriveWorkload::Random);
    engine.setFlood(true);
    a = Acc{}; run(2, a); line("flood", a);
    engine.setFlood(false);
    engine.setWorkload(DriveWorkload::Idle);
    engine.power(false);
    a = Acc{}; run(p.coastS + 2, a);
    std::snprintf(extra, sizeof(extra), "  rpm %u, stopped %d", unsigned(engine.rpm()), engine.stopped());
    line("off", a, extra);
    if (out) writeWav(out);
    if (failed) { std::printf("NaN or infinity in the output\n"); return 1; }
    return 0;
}
