// The modelled drive: a physics model of the spindle and the head, and the
// real-time synthesis of its sound. Pure C++ (no Arduino), host-tested.
// A port of the browser prototype's engine (tools/drive-lab/engine.js):
// keep the two in step.
//
// It renders as a voice source: mixBlock() calls render() on core 1, from
// RAM only, never blocking. Core 0 drives it with the commands below, which
// go through a lock-free SPSC ring and are applied at the start of the next
// rendered block (every 8 ms or so):
//
//   begin()          core 0, only while the voice is idle (or before it starts)
//   power(), ...     core 0, any time; false if the ring is full (retry later)
//   render()         core 1 (the consumer)
//   state(), rpm()   any core: telemetry, published by core 1 once per block
//
// About 2.9 KB: the firmware allocates it only while a modelled pack is
// loaded, so the RAM is free for WAV packs' spindowns otherwise.
//
// What it models, in layers (each with its own gain in DriveParams):
//   spindle  rotation hum at rpm/60 Hz, motor whine at rpm/60 x slots (loud
//            while the motor works hard), the electrical frequency
//   bearing  band-passed noise plus a defect tone; landing-zone heads scrape
//            as the platters slow
//   air      windage: filtered noise rising with rpm, platter size and count
//   actuator a voice coil seeking bang-bang (full force, then full braking)
//            or a stepper's steps, ringing the arm and case modes
//   servo    a faint buzz at the servo wedge rate while transferring
// then the enclosure EQ and a soft clip.
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "drive_params.h"
#include "ring_buffer.h"

enum class DriveWorkload : uint8_t { Idle, Sequential, Random, Boot, Defrag };
enum class DriveState : uint8_t { Off, Spinup, Calibrate, Ready };

class DriveEngine {
public:
    static constexpr int SINE_N = 256;  // the sine table's size
    // ---- core 0, voice idle ----
    // Reset to a stopped drive with its heads parked, at `sampleRate`.
    void begin(uint32_t sampleRate, const DriveParams& p);

    // ---- core 0, any time (queued for core 1) ----
    // Power on: spin up, unpark, calibrate, then Ready. `atSpeed` skips
    // straight to Ready (a pack change while running). Power off: park the
    // heads and coast down; stopped() once it has.
    bool power(bool on, bool atSpeed = false);
    // What the host is doing: the virtual workload makes the head's targets.
    bool setWorkload(DriveWorkload w);
    // Balance: 0 = only the drive's running sound (spindle, bearing, air),
    // 1 = only its seeks (actuator, servo), 0.5 = both at full. With
    // `idleSound` off, the running sound is silent once Ready (PLAY_IDLE).
    bool setBalance(float balance, bool idleSound);
    // Seek the head to `x` across the stroke (0 = outer edge, 1 = inner)
    // and hold it there, in place of the workload. For the link's positions.
    bool seekTo(float x);
    // A new target every 2 ms whatever the head is doing, like an emulated
    // drive answering from SD (the head merges them).
    bool setFlood(bool on);
    // New parameters, applied at the next block. False while the previous
    // update is still pending: try again a block later.
    bool updateParams(const DriveParams& p);

    // ---- core 1 ----
    void render(int16_t* out, size_t frames);

    // ---- telemetry (any core) ----
    DriveState state() const { return static_cast<DriveState>(telState_.load(std::memory_order_acquire)); }
    uint32_t rpm() const { return telRpm_.load(std::memory_order_relaxed); }
    // Powered off, spun down to under 2% of its speed and the heads parked.
    bool stopped() const { return telStopped_.load(std::memory_order_acquire); }
    bool busy() const { return telBusy_.load(std::memory_order_relaxed); }
    uint32_t seekCount() const { return telSeeks_.load(std::memory_order_relaxed); }
    uint32_t lastSeekUs() const { return telLastSeekUs_.load(std::memory_order_relaxed); }
    uint32_t cylinder() const { return telCylinder_.load(std::memory_order_relaxed); }

    // Host tests and tools: render to float, before the int16 conversion.
    void renderFloat(float* out, size_t frames);

private:
    struct Svf { float a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0, lp = 0, bp = 0; };
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; };
    enum class Cmd : uint8_t { Power, Workload, Balance, SeekTo, Flood };
    struct Event { Cmd cmd; uint8_t a; uint8_t b; float value; };
    struct Step { bool hasX; float x, click, caseKick, wait; };
    enum class JobKind : uint8_t { Seek, Seq, Pause };
    struct Job { JobKind kind; bool save, ret; float f, ms; };
    static constexpr int MODES = 12;  // 8 arm modes + 4 case modes
    static constexpr int MAX_STEPS = 24;
    static constexpr int MAX_JOBS = 8;

    bool post(const Event& e);
    void applyEvent(const Event& e);
    void applyParams(bool actuatorChanged);
    void tuneModes();
    void tuneEq();
    void control();
    void runJobs();
    void refillJobs();
    void seekHead(float xs, float tick = -1);
    bool headSettled() const;
    float mapX(float f) const;
    float parkX() const;
    bool stepper() const { return p_.actuator == Actuator::Stepper; }
    int cylinders() const;
    void click(float amount, float caseAmount = 0) { clickAcc_ += amount; caseAcc_ += caseAmount; }
    float rnd();
    float noise() __attribute__((always_inline)) { return rnd() * 2 - 1; }
    void scriptCalibration();
    void scriptRecal();
    void addStep(bool hasX, float x, float click, float caseKick, float wait);
    void addJob(JobKind k, float f, float ms, bool save = false, bool ret = false);
    void publish();
    void tuneSvf(Svf& s, float fc, float q);
    static void runSvf(Svf& s, float x);
    static void setBiquad(Biquad& f, int type, float freq, float q, float db, float sr);
    static float runBiquad(Biquad& f, float x);

    // Core 0 -> core 1
    SpscRing<Event, 32> events_;
    DriveParams pending_;
    std::atomic<bool> paramsPending_{false};

    // Telemetry, core 1 -> any
    std::atomic<uint8_t> telState_{0};
    std::atomic<uint32_t> telRpm_{0};
    std::atomic<bool> telStopped_{true};
    std::atomic<bool> telBusy_{false};
    std::atomic<uint32_t> telSeeks_{0};
    std::atomic<uint32_t> telLastSeekUs_{0};
    std::atomic<uint32_t> telCylinder_{0};

    // Everything below belongs to core 1 once rendering (core 0 sets it in begin()).
    float sine_[SINE_N + 1];
    DriveParams p_;
    uint32_t rate_ = 32000;
    float sr_ = 32000, dt_ = 1.0f / 32000;
    int ctrlEvery_ = 32, ctrlCount_ = 0;
    float ctrlDt_ = 0.001f;
    uint64_t sampleCount_ = 0;  // the clock, in samples (no doubles: the FPU is single precision)
    uint32_t seed_ = 0x2545F491;

    // Spindle and power
    bool powered_ = false;
    DriveState state_ = DriveState::Off;
    float rpm_ = 0, motorCurrent_ = 0;
    float phRot_ = 0, phWhine_ = 0, phElec_ = 0, phBear_ = 0, phServo_ = 0;
    float humAmp_ = 0, whineAmp_ = 0, elecAmp_ = 0, windAmp_ = 0, bearAmp_ = 0, bearToneAmp_ = 0, scrapeAmp_ = 0;
    float servoGate_ = 0;
    float tunedRpm_ = -1;          // the speed the noise filters were last tuned for
    bool paramsChanged_ = true;
    Svf windLp_, windBp_, bearBp_, servoLp_, scrapeBp_;
    float balance_ = 0.5f;
    bool idleSound_ = true;

    // Head (voice coil). x: 0 = outer edge (LBA 0), 1 = inner edge.
    float x_ = 0, v_ = 0, xt_ = 0, aPrev_ = 0;
    bool seeking_ = false;  // bang-bang seek; otherwise track following / settling
    float A_ = 1e4f, w_ = 4000, zeta_ = 0.5f;
    bool retract_ = false;
    // Head (stepper)
    int track_ = 0, tTrack_ = 0, stepsDone_ = 0;
    float stepTimer_ = 0, stSettle_ = 0, xPrev_ = 0;
    // Modal bank
    float mc1_[MODES] = {}, mc2_[MODES] = {}, mg_[MODES] = {}, my1_[MODES] = {}, my2_[MODES] = {};
    float excLp_ = 0, excK_ = 0.5f, clickAcc_ = 0, caseAcc_ = 0;
    Biquad eqLow_, eqCase_, eqHigh_;

    // Bookkeeping
    int settledMs_ = 0;
    bool seekPending_ = false, parkPending_ = false;
    uint64_t seekStart_ = 0;
    float seekD_ = 0;
    uint32_t seekCount_ = 0, lastSeekUs_ = 0;

    // Scripts (calibration, recalibration) and the workload's jobs
    Step steps_[MAX_STEPS];
    int stepHead_ = 0, stepCount_ = 0;
    float stepWait_ = 0;
    Job jobs_[MAX_JOBS];
    int jobHead_ = 0, jobCount_ = 0;
    Job job_{};
    bool haveJob_ = false, jobStarted_ = false;
    float jobT_ = 0, jobLat_ = 0, retX_ = 0, seqAcc_ = 0, recalTimer_ = 0;
    DriveWorkload workload_ = DriveWorkload::Idle;
    bool flood_ = false, busy_ = false;
};
