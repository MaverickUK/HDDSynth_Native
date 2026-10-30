#include "drive_engine.h"

#include <math.h>
#include <new>

// The per-sample path runs from RAM on the RP2350: from flash, its loop
// misses the XIP cache (shared with core 0) and stalls on flash reads.
// Arduino-Pico copies `.time_critical.*` sections to RAM at boot.
// The small helpers must be inlined: left out of line, the compiler puts
// them in flash, and the RAM loop calls them through flash every sample.
#if defined(__arm__)
#define DRIVE_RAM __attribute__((section(".time_critical.drive")))
#else
#define DRIVE_RAM
#endif
#define DRIVE_INLINE inline __attribute__((always_inline))

namespace {

constexpr float TAU = 6.2831853f;
constexpr float SETTLE_WIN = 0.003f;  // within this of the target the seek hands over to track following
constexpr float RI = 0.45f;           // inner/outer radius of the data zone
constexpr float ARM[8] = {1, 1.62f, 2.31f, 2.97f, 3.83f, 4.7f, 6.1f, 7.9f};
constexpr float CASE[4] = {1, 1.47f, 2.09f, 2.83f};

// Sine by table (DriveEngine::sine_): one cycle per 1.0 of phase, linearly
// interpolated. Far cheaper than sinf() on the RP2350, and plenty clean for
// these tones (the error is about -94 dB).
constexpr int SINE_N = DriveEngine::SINE_N;

DRIVE_INLINE float sin1(const float* lut, float ph) {  // ph in [0, 1)
    const float f = ph * SINE_N;
    int i = static_cast<int>(f);
    const float fr = f - i;
    i &= SINE_N - 1;
    return lut[i] + (lut[i + 1] - lut[i]) * fr;
}

// Square root on the FPU (vsqrt.f32). newlib's sqrtf() keeps errno, so it
// calls a software routine instead (and links ~5 KB of float helpers into
// RAM); the seek controller needs one every sample. Arguments are never
// negative here.
DRIVE_INLINE float sqrtFast(float x) {
#if defined(__arm__) && defined(__ARM_FP)
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
#else
    return sqrtf(x);
#endif
}

DRIVE_INLINE float wrap(float ph) { return ph >= 1 ? ph - 1 : ph; }
DRIVE_INLINE float twice(float ph) { return wrap(ph * 2); }  // the second harmonic's phase
DRIVE_INLINE float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// tanh, near enough for a soft clip (exact at 0, saturates at +-3).
DRIVE_INLINE float softClip(float x) {
    x = clampf(x, -3, 3);
    return x * (27 + x * x) / (27 + 9 * x * x);
}

DRIVE_INLINE float clickAmp(float d) { return 0.15f + 0.85f * sqrtFast(fminf(1, d / 0.25f)); }

const float SPOTS[8] = {0.98f, 0.02f, 0.5f, 0.25f, 0.75f, 0.1f, 0.9f, 0.4f};

} // namespace

// ---- core 0 ---------------------------------------------------------------

void DriveEngine::begin(uint32_t sampleRate, const DriveParams& p) {
    // Fresh state, the ring and atomics included (they can't be assigned):
    // rebuild the object in place. Safe because core 1 isn't rendering it.
    this->~DriveEngine();
    new (this) DriveEngine();
    for (int i = 0; i <= SINE_N; ++i) sine_[i] = sinf(TAU * i / SINE_N);
    rate_ = sampleRate;
    sr_ = static_cast<float>(sampleRate);
    dt_ = 1.0f / sr_;
    ctrlEvery_ = static_cast<int>(lroundf(sr_ / 1000));
    ctrlDt_ = ctrlEvery_ / sr_;
    tuneSvf(servoLp_, 5000, 0.7f);
    p_ = p;
    applyParams(false);
    x_ = xt_ = parkX();  // heads parked (a stepper's sit wherever they are: track 0)
    publish();
}

bool DriveEngine::post(const Event& e) { return events_.push(&e, 1) == 1; }

bool DriveEngine::power(bool on, bool atSpeed) { return post({Cmd::Power, uint8_t(on), uint8_t(atSpeed), 0}); }
bool DriveEngine::setWorkload(DriveWorkload w) { return post({Cmd::Workload, uint8_t(w), 0, 0}); }
bool DriveEngine::setBalance(float balance, bool idleSound) {
    return post({Cmd::Balance, uint8_t(idleSound), 0, clampf(balance, 0, 1)});
}
bool DriveEngine::seekTo(float x) { return post({Cmd::SeekTo, 0, 0, clampf(x, 0, 1)}); }
bool DriveEngine::setFlood(bool on) { return post({Cmd::Flood, uint8_t(on), 0, 0}); }

bool DriveEngine::updateParams(const DriveParams& p) {
    if (paramsPending_.load(std::memory_order_acquire)) return false;
    pending_ = p;  // core 1 doesn't read it until the flag below is set
    paramsPending_.store(true, std::memory_order_release);
    return true;
}

// ---- core 1: commands and parameters ------------------------------------------

void DriveEngine::applyEvent(const Event& e) {
    switch (e.cmd) {
        case Cmd::Power: {
            const bool on = e.a != 0;
            if (on == powered_) break;
            powered_ = on;
            stepCount_ = 0;
            haveJob_ = false;
            jobCount_ = 0;
            if (on && e.b) {  // straight to speed, heads on the disk
                rpm_ = p_.rpm;
                motorCurrent_ = 0.15f;
                if (!stepper()) { x_ = xt_ = 0.01f; v_ = 0; seeking_ = false; }
                state_ = DriveState::Ready;
            } else if (on) {
                state_ = DriveState::Spinup;
                if (stepper()) click(0.6f, 1.0f);  // the stepper energises: one thunk
            } else {
                state_ = DriveState::Off;
                if (!stepper() && p_.park != Parking::None) {
                    retract_ = true;
                    parkPending_ = true;
                    seekHead(parkX());
                }
            }
            break;
        }
        case Cmd::Workload:
            workload_ = static_cast<DriveWorkload>(e.a);
            haveJob_ = false;
            jobCount_ = 0;
            break;
        case Cmd::Balance:
            balance_ = e.value;
            idleSound_ = e.a != 0;
            break;
        case Cmd::SeekTo:
            if (state_ == DriveState::Ready) {
                stepCount_ = 0;
                haveJob_ = false;
                jobCount_ = 0;
                workload_ = DriveWorkload::Idle;
                seekHead(e.value);
            }
            break;
        case Cmd::Flood:
            flood_ = e.a != 0;
            break;
    }
}

void DriveEngine::applyParams(bool actuatorChanged) {
    const float T = fmaxf(0.004f, p_.fullStrokeMs / 1000);
    A_ = 4.6f / (T * T);  // bang-bang: a full stroke in about T
    zeta_ = clampf(p_.settleDamp, 0.05f, 1.5f);
    w_ = 4 / (zeta_ * fmaxf(0.0003f, p_.trackMs / 1000));
    excK_ = 1 - expf(-TAU * (1500 + p_.hardness * 14000) / sr_);
    tuneModes();
    tuneEq();
    paramsChanged_ = true;  // retune the noise filters too
    const int cyl = cylinders();
    if (actuatorChanged) {  // carry the head's position over to the other actuator
        track_ = tTrack_ = static_cast<int>(lroundf(clampf(x_, 0, 1) * (cyl - 1)));
        x_ = xt_ = static_cast<float>(track_) / (cyl - 1);
        v_ = 0;
        seeking_ = false;
    }
    if (track_ > cyl - 1) track_ = cyl - 1;
    if (tTrack_ > cyl - 1) tTrack_ = cyl - 1;
}

void DriveEngine::tuneModes() {
    for (int i = 0; i < MODES; ++i) {
        const bool arm = i < 8;
        const int j = arm ? i : i - 8;
        const float f = arm ? p_.armHz * ARM[j] : p_.caseHz * CASE[j];
        if (f > sr_ * 0.42f) { mg_[i] = mc1_[i] = mc2_[i] = 0; continue; }
        const float t60 = arm ? (p_.armRingMs / 1000) / (1 + j * 0.25f) : (p_.caseRingMs / 1000) / (1 + j * 0.3f);
        const float amp = arm ? 1 / (1 + 0.35f * j) : 0.9f / (1 + 0.5f * j);
        const float r = expf(-6.91f / (t60 * sr_)), w = TAU * f / sr_;
        mc1_[i] = 2 * r * cosf(w);
        mc2_[i] = -r * r;
        mg_[i] = amp * sinf(w);
    }
}

void DriveEngine::tuneEq() {
    const float lowDb = p_.mount == Mount::Bare ? -5 : (p_.mount == Mount::Sled ? 1 : 4);
    setBiquad(eqLow_, 0, 220, 0.707f, lowDb, sr_);
    setBiquad(eqCase_, 1, p_.caseHz * 0.8f, 1.6f, p_.caseRes * 9 * (p_.mount == Mount::Bare ? 0.4f : 1), sr_);
    setBiquad(eqHigh_, 2, 3500, 0.707f, p_.bright * 9 - (p_.mount == Mount::Case ? 2 : 0), sr_);
}

// ---- filters --------------------------------------------------------------------

// State-variable filter (TPT form): stays stable while its cutoff moves.
DRIVE_RAM void DriveEngine::tuneSvf(Svf& s, float fc, float q) {
    const float sr = sr_;
    // g = tan(pi fc / sr), as sin/cos from the table: tanf() from the
    // library costs far more, and this runs at the control rate.
    const float ph = clampf(fc, 10, sr * 0.45f) / (2 * sr);  // pi fc/sr as a fraction of a cycle
    const float g = sin1(sine_, ph) / sin1(sine_, ph + 0.25f);
    const float k = 1 / q;
    s.a1 = 1 / (1 + g * (g + k));
    s.a2 = g * s.a1;
    s.a3 = g * s.a2;
}

DRIVE_INLINE void DriveEngine::runSvf(Svf& s, float x) {
    const float v3 = x - s.ic2, v1 = s.a1 * s.ic1 + s.a2 * v3, v2 = s.ic2 + s.a2 * s.ic1 + s.a3 * v3;
    s.ic1 = 2 * v1 - s.ic1;
    s.ic2 = 2 * v2 - s.ic2;
    s.lp = v2;
    s.bp = v1;
}

// RBJ biquads: type 0 low shelf, 1 peak, 2 high shelf.
void DriveEngine::setBiquad(Biquad& f, int type, float freq, float q, float db, float sr) {
    const float A = powf(10, db / 40), w = TAU * fminf(freq, sr * 0.45f) / sr;
    const float cw = cosf(w), sw = sinf(w), al = sw / (2 * q);
    float b0, b1, b2, a0, a1, a2;
    if (type == 1) {
        b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
    } else {
        const float s = 2 * sqrtFast(A) * al;
        if (type == 0) {
            b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
        }
    }
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
}

DRIVE_INLINE float DriveEngine::runBiquad(Biquad& f, float x) {
    const float y = f.b0 * x + f.z1;
    f.z1 = f.b1 * x - f.a1 * y + f.z2;
    f.z2 = f.b2 * x - f.a2 * y;
    return y;
}

// ---- the head -------------------------------------------------------------------

DRIVE_INLINE float DriveEngine::rnd() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 17;
    seed_ ^= seed_ << 5;
    return static_cast<float>(seed_ >> 8) * (1.0f / 16777216.0f);  // 24 bits: all a float holds
}

int DriveEngine::cylinders() const {
    const int c = static_cast<int>(p_.cylinders);
    return c < 2 ? 2 : c;
}

float DriveEngine::parkX() const {
    if (p_.park == Parking::Ramp) return -0.06f;  // off the outer edge, on the ramp
    if (p_.park == Parking::LandingZone) return 1.04f;  // inside the data zone
    return x_;
}

// Zoned recording: constant areal density, so LBA fraction f covers the
// area outside radius r. Without it, tracks are evenly filled.
float DriveEngine::mapX(float f) const {
    f = clampf(f, 0, 1);
    if (!p_.zoned) return f;
    const float r = sqrtFast(1 - f * (1 - RI * RI));
    return (1 - r) / (1 - RI);
}

DRIVE_RAM void DriveEngine::seekHead(float xs, float tick) {
    if (stepper()) {
        const int cyl = cylinders();
        int nt = static_cast<int>(lroundf(clampf(xs, 0, 1) * (cyl - 1)));
        if (nt > cyl - 1) nt = cyl - 1;
        if (nt == tTrack_) return;
        seekD_ = static_cast<float>(nt > track_ ? nt - track_ : track_ - nt) / (cyl - 1);
        tTrack_ = nt;
        stepsDone_ = 0;
    } else {
        const float d = fabsf(xs - x_);
        xt_ = xs;
        seekD_ = d;
        if (d > SETTLE_WIN) {
            seeking_ = true;
            click(clickAmp(d) * 0.6f);
        } else {
            click(clickAmp(d) * (tick < 0 ? 1 : tick));
        }
    }
    seekPending_ = true;
    seekStart_ = sampleCount_;
}

DRIVE_RAM bool DriveEngine::headSettled() const {
    if (stepper()) return track_ == tTrack_ && stepTimer_ <= 0 && stSettle_ <= 0;
    return !seeking_ && settledMs_ >= 1;
}

// ---- scripts and jobs --------------------------------------------------------------

void DriveEngine::addStep(bool hasX, float x, float clk, float caseKick, float wait) {
    if (stepCount_ >= MAX_STEPS) return;
    steps_[(stepHead_ + stepCount_) % MAX_STEPS] = {hasX, x, clk, caseKick, wait};
    ++stepCount_;
}

void DriveEngine::scriptCalibration() {
    stepHead_ = stepCount_ = 0;
    stepWait_ = 0;
    if (stepper()) {
        addStep(true, 0, 0, 0, 0);  // recalibrate: step out to track 0
        addStep(false, 0, 0, 0, 0.05f);
    } else if (p_.park != Parking::None) {
        addStep(false, 0, 0.5f, 0.9f, 0);  // latch release
        addStep(false, 0, 0, 0, 0.03f);
        addStep(true, p_.park == Parking::Ramp ? 0.01f : 0.96f, 0, 0, 0);  // off the ramp / the landing zone
        addStep(false, 0, 0, 0, 0.04f);
    }
    const int n = static_cast<int>(p_.calSeeks);
    for (int i = 0; i < n; ++i) {
        addStep(true, SPOTS[i % 8], 0, 0, 0);
        addStep(false, 0, 0, 0, 0.03f + 0.05f * rnd());
    }
    addStep(true, 0.01f, 0, 0, 0);
}

void DriveEngine::scriptRecal() {
    stepHead_ = stepCount_ = 0;
    stepWait_ = 0;
    addStep(true, 0.95f, 0, 0, 0);
    addStep(false, 0, 0, 0, 0.02f);
    addStep(true, 0.05f, 0, 0, 0);
    addStep(false, 0, 0, 0, 0.02f);
    addStep(true, 0.6f, 0, 0, 0);
    addStep(false, 0, 0, 0, 0.03f);
    addStep(true, 0.3f, 0, 0, 0);
}

void DriveEngine::addJob(JobKind k, float f, float ms, bool save, bool ret) {
    if (jobCount_ >= MAX_JOBS) return;
    jobs_[(jobHead_ + jobCount_) % MAX_JOBS] = {k, save, ret, f, ms};
    ++jobCount_;
}

// The virtual workload: what a host doing this would ask the drive for.
void DriveEngine::refillJobs() {
    switch (workload_) {
        case DriveWorkload::Random:
            addJob(JobKind::Seek, rnd(), 0);
            break;
        case DriveWorkload::Sequential:  // a file copy: long runs, with a trip to the FAT now and then
            addJob(JobKind::Seq, 0, 300 + rnd() * 400);
            addJob(JobKind::Seek, 0.002f, 0, /*save*/ true);
            addJob(JobKind::Seq, 0, 10);
            addJob(JobKind::Seek, 0, 0, false, /*ret*/ true);
            break;
        case DriveWorkload::Boot: {
            addJob(JobKind::Seek, 0.001f, 0);
            addJob(JobKind::Seq, 0, 8);
            addJob(JobKind::Seek, 0.002f + rnd() * 0.3f, 0);
            addJob(JobKind::Seq, 0, 40 + rnd() * 260);
            const float pause = rnd() < 0.3f ? 200 + rnd() * 500 : rnd() * 60;
            addJob(JobKind::Pause, 0, pause);
            break;
        }
        case DriveWorkload::Defrag:
            addJob(JobKind::Seek, 0.4f + rnd() * 0.55f, 0);
            addJob(JobKind::Seq, 0, 120 + rnd() * 80);
            addJob(JobKind::Seek, 0.03f + rnd() * 0.2f, 0);
            addJob(JobKind::Seq, 0, 120 + rnd() * 80);
            addJob(JobKind::Seek, 0.001f, 0);
            addJob(JobKind::Seq, 0, 6);
            break;
        default:
            addJob(JobKind::Pause, 0, 50);
    }
}

DRIVE_RAM void DriveEngine::runJobs() {
    if (!haveJob_) {
        if (!jobCount_) refillJobs();
        job_ = jobs_[jobHead_];
        jobHead_ = (jobHead_ + 1) % MAX_JOBS;
        --jobCount_;
        haveJob_ = true;
        jobT_ = 0;
        jobStarted_ = false;
    }
    const float fr = rpm_ / 60;
    const int cyl = cylinders();
    switch (job_.kind) {
        case JobKind::Seek:
            if (!jobStarted_) {
                if (job_.save) retX_ = stepper() ? static_cast<float>(tTrack_) / (cyl - 1) : xt_;
                seekHead(job_.ret ? retX_ : mapX(job_.f));
                jobStarted_ = true;
                jobLat_ = (fr > 0 ? rnd() / fr : 0) + 0.0004f;  // rotational latency, then the transfer
            }
            busy_ = true;
            if (flood_) {
                if (jobT_ >= 0.002f) haveJob_ = false;
            } else if (headSettled()) {
                jobLat_ -= ctrlDt_;
                if (jobLat_ <= 0) haveJob_ = false;
            }
            break;
        case JobKind::Seq: {
            busy_ = true;
            // One cylinder per (heads) revolutions: the drive switches heads first.
            const float cylTime = fr > 0 ? (2 * p_.platters) / fr : 1;
            seqAcc_ += ctrlDt_;
            if (seqAcc_ >= cylTime) {
                seqAcc_ -= cylTime;
                const float step = 1.0f / (cyl - 1);
                const float cur = stepper() ? static_cast<float>(tTrack_) / (cyl - 1) : xt_;
                if (cur + step <= 1) seekHead(cur + step, p_.tick);
            }
            if (jobT_ >= job_.ms / 1000) haveJob_ = false;
            break;
        }
        case JobKind::Pause:
            if (jobT_ >= job_.ms / 1000) haveJob_ = false;
            break;
    }
    jobT_ += ctrlDt_;
}

// ---- the control tick (1 kHz) -------------------------------------------------------

DRIVE_RAM void DriveEngine::control() {
    // Spindle speed: motor torque up, windage and friction down.
    if (powered_) {
        if (rpm_ < p_.rpm) {
            rpm_ += (p_.rpm / p_.spinupS) * ctrlDt_ * (1.25f - 0.5f * rpm_ / p_.rpm);
            motorCurrent_ += (1 - motorCurrent_) * 0.05f;
            if (rpm_ > p_.rpm) rpm_ = p_.rpm;
        } else {
            rpm_ += (p_.rpm - rpm_) * 0.02f;
            motorCurrent_ += (0.15f - motorCurrent_) * 0.01f;
        }
    } else {
        // Windage (proportional to speed) plus bearing friction (constant):
        // drpm/dt = -rpm/tau - c. With tau = coastS/2.5, this c stops it at
        // exactly coastS: tau * ln(1 + rpm/(c tau)) = coastS.
        const float tau = p_.coastS / 2.5f;
        rpm_ -= ctrlDt_ * (rpm_ / tau + 0.2236f * p_.rpm / p_.coastS);
        if (rpm_ < 0) rpm_ = 0;
        motorCurrent_ *= 0.97f;
    }
    const float sn = p_.rpm > 0 ? rpm_ / p_.rpm : 0, sa = rpm_ / 7200;
    const float sizeF = p_.size == FormFactor::In525 ? 1.5f : (p_.size == FormFactor::In25 ? 0.7f : 1);
    const bool ball = p_.bearing == Bearing::Ball;

    humAmp_ = p_.hum * 0.05f * fminf(1, sn * 3);
    whineAmp_ = p_.whine * 0.035f * (0.3f + 1.7f * motorCurrent_) * fminf(1, sn * 4);
    elecAmp_ = p_.whine * 0.012f * motorCurrent_ * fminf(1, sn * 6);
    windAmp_ = p_.windage * 0.22f * sqrtFast(p_.platters / 2) * sizeF * sa * sa;
    // The noise filters follow the spindle speed; at a steady speed they stay put.
    const bool retune = fabsf(rpm_ - tunedRpm_) >= 1 || paramsChanged_;
    if (retune) {
        tunedRpm_ = rpm_;
        paramsChanged_ = false;
        tuneSvf(windLp_, 120 + 2600 * sa * sizeF, 0.6f);
        tuneSvf(windBp_, 250 + 1200 * sa * sizeF, 1.3f);
    }
    bearAmp_ = p_.bearingLvl * (ball ? 0.05f : 0.012f) * (0.3f + p_.wear) * (sn * sqrtFast(sn));
    bearToneAmp_ = p_.bearingLvl * (ball ? 0.012f : 0.003f) * p_.wear * sn;
    if (retune) tuneSvf(bearBp_, (ball ? 2400 : 1600) * (0.4f + 0.6f * sn) * (1 + 0.3f * p_.wear), ball ? 2.5f : 1.2f);
    // Contact start-stop heads touching down as the platters slow.
    const bool landing = p_.park == Parking::LandingZone && !stepper() && !powered_ && sn > 0.02f && sn < 0.35f;
    scrapeAmp_ = landing ? 0.05f * (1 - sn / 0.35f) * p_.bearingLvl : 0;
    if (retune && landing) tuneSvf(scrapeBp_, 900 + 3000 * sn, 1.5f);

    // Head settled?
    if (!stepper()) {
        if (!seeking_ && fabsf(x_ - xt_) < 0.0006f && fabsf(v_) < 0.08f) ++settledMs_;
        else settledMs_ = 0;
    } else if (stSettle_ > 0) {
        stSettle_ -= ctrlDt_;
    }
    if (seekPending_ && headSettled()) {
        seekPending_ = false;
        lastSeekUs_ = static_cast<uint32_t>((sampleCount_ - seekStart_) * 1000000ull / rate_);  // integers: no float-to-64-bit helpers
        ++seekCount_;
        if (parkPending_) {
            parkPending_ = false;
            retract_ = false;
            if (!stepper()) click(0.7f, 1.0f);  // the latch catches the arm
        }
    }

    // Power sequencing
    if (state_ == DriveState::Spinup && rpm_ >= p_.rpm * 0.97f) {
        state_ = DriveState::Calibrate;
        scriptCalibration();
    }
    if (stepCount_) {
        if (stepWait_ > 0) {
            stepWait_ -= ctrlDt_;
        } else if (headSettled() || !steps_[stepHead_].hasX) {
            const Step s = steps_[stepHead_];
            stepHead_ = (stepHead_ + 1) % MAX_STEPS;
            --stepCount_;
            if (s.hasX) seekHead(s.x);
            if (s.click > 0) click(s.click, s.caseKick);
            if (s.wait > 0) stepWait_ = s.wait;
        }
    } else if (state_ == DriveState::Calibrate) {
        state_ = DriveState::Ready;
        haveJob_ = false;
        jobCount_ = 0;
    }

    busy_ = false;
    if (state_ == DriveState::Ready && !stepCount_) {
        if (workload_ == DriveWorkload::Idle || (haveJob_ && job_.kind == JobKind::Pause)) {
            recalTimer_ += ctrlDt_;
            if (p_.recalS > 0 && recalTimer_ > p_.recalS && headSettled()) {
                recalTimer_ = 0;
                scriptRecal();
            }
        } else {
            recalTimer_ = 0;
        }
        if (workload_ != DriveWorkload::Idle && !stepCount_) runJobs();
    }
    servoGate_ += ((busy_ ? 1 : 0) - servoGate_) * 0.05f;
}

// ---- rendering ---------------------------------------------------------------------

DRIVE_RAM void DriveEngine::renderFloat(float* out, size_t frames) {
    if (paramsPending_.load(std::memory_order_acquire)) {
        const bool actuatorChanged = pending_.actuator != p_.actuator;
        p_ = pending_;
        applyParams(actuatorChanged);
        paramsPending_.store(false, std::memory_order_release);
    }
    Event e;
    while (events_.pop(&e, 1) == 1) applyEvent(e);

    const int cyl = cylinders();
    const float* lut = sine_;
    const float idleMul = fminf(1, 2 * (1 - balance_)) * (idleSound_ || state_ != DriveState::Ready ? 1 : 0);
    const float accMul = fminf(1, 2 * balance_);
    const float gSpin = p_.gSpindle * idleMul, gBear = p_.gBearing * idleMul, gWind = p_.gWind * idleMul;
    const float gAct = p_.gAct * accMul, gServo = p_.gServo * accMul;

    for (size_t i = 0; i < frames; ++i) {
        if (++ctrlCount_ >= ctrlEvery_) {
            ctrlCount_ = 0;
            control();
        }
        ++sampleCount_;
        const float fr = rpm_ / 60;

        // Spindle: rotation hum, the motor's slot whine and its electrical frequency.
        phRot_ = wrap(phRot_ + fr * dt_);
        phWhine_ = wrap(phWhine_ + fr * p_.slots * dt_);
        phElec_ = wrap(phElec_ + fr * p_.polePairs * dt_);
        const float rot = sin1(lut, phRot_);
        const float am = 1 + p_.imbalance * 0.5f * rot;
        const float spin = (humAmp_ * (rot + 0.5f * sin1(lut, twice(phRot_)))
                            + whineAmp_ * (sin1(lut, phWhine_) + 0.35f * sin1(lut, twice(phWhine_)))
                            + elecAmp_ * sin1(lut, phElec_)) * am;

        // Bearing: band-passed noise plus a defect tone; CSS heads scraping while landing.
        phBear_ = wrap(phBear_ + fr * 4.37f * dt_);
        runSvf(bearBp_, noise());
        float bear = bearBp_.bp * bearAmp_ * am + bearToneAmp_ * sin1(lut, phBear_);
        if (scrapeAmp_ > 0) {
            runSvf(scrapeBp_, noise());
            bear += scrapeBp_.bp * scrapeAmp_;
        }

        // Windage: air over the platters.
        runSvf(windLp_, noise());
        runSvf(windBp_, noise());
        const float wind = (windLp_.lp * 0.7f + windBp_.bp * 0.6f) * windAmp_ * (1 + p_.imbalance * 0.15f * rot);

        // Actuator
        float exc = 0;
        if (stepper()) {
            if (stepTimer_ > 0) stepTimer_ -= 1;
            if (stepTimer_ <= 0 && track_ != tTrack_) {
                track_ += track_ < tTrack_ ? 1 : -1;
                ++stepsDone_;
                const int remaining = tTrack_ > track_ ? tTrack_ - track_ : track_ - tTrack_;
                const int k = stepsDone_ < remaining + 1 ? stepsDone_ : remaining + 1;
                // Buffered seek: the step rate ramps up, then down again into the target.
                const float ms = p_.stepMs * (1 + 4 * p_.stepRamp * expf(-k / (3 + 12 * p_.stepRamp)));
                stepTimer_ += ms / 1000 * sr_;  // += keeps the fraction: no drift over a long seek
                exc += 0.55f * (0.7f + 0.3f * rnd());
                if (remaining == 0) {
                    stSettle_ = 0.012f;
                    caseAcc_ += 0.35f;
                }
            } else if (stepTimer_ < 0) {
                stepTimer_ = 0;
            }
            const float xs = static_cast<float>(track_) / (cyl - 1);
            x_ += (xs - x_) * 0.01f;
            v_ = (x_ - xPrev_) * sr_;
            xPrev_ = x_;
        } else {
            const float d = xt_ - x_;
            if (seeking_ && fabsf(d) < SETTLE_WIN) {
                seeking_ = false;
                click(clickAmp(seekD_) * 0.45f, 0.15f * clickAmp(seekD_));
            }
            const float Ar = retract_ ? A_ * 1.5f : A_;
            float a;
            if (seeking_) {
                // Full force towards the target, braking along the curve that stops on it.
                const float vdes = (d > 0 ? 1 : -1) * sqrtFast(2 * Ar * 0.8f * fabsf(d));
                a = clampf((vdes - v_) * 4000, -Ar, Ar);
            } else {
                a = clampf(w_ * w_ * d - 2 * zeta_ * w_ * v_, -Ar, Ar);  // the servo loop
            }
            v_ += a * dt_;
            x_ += v_ * dt_;
            exc += (a - aPrev_) / A_ * 0.5f;           // force changes kick the arm
            exc += p_.buzz * fabsf(a) / A_ * noise() * 0.08f;  // and the coil buzzes under load
            aPrev_ = a;
        }
        exc += clickAcc_;
        clickAcc_ = 0;
        excLp_ += (exc - excLp_) * excK_;
        const float caseIn = caseAcc_;
        caseAcc_ = 0;
        float act = 0;
        for (int m = 0; m < MODES; ++m) {
            const float in = m < 8 ? excLp_ : excLp_ * 0.4f + caseIn;
            const float y = mc1_[m] * my1_[m] + mc2_[m] * my2_[m] + mg_[m] * in;
            my2_[m] = my1_[m];
            my1_[m] = y;
            act += y;
        }
        act *= 0.16f;

        // Servo: the track-following buzz at the servo wedge rate while transferring.
        float servo = 0;
        if (p_.wedges > 0 && servoGate_ > 0.01f) {
            phServo_ = wrap(phServo_ + fr * p_.wedges * dt_);
            runSvf(servoLp_, phServo_ < 0.1f ? 1.0f : 0.0f);
            servo = servoLp_.bp * p_.servo * 0.25f * servoGate_;
        }

        float mix = spin * gSpin + bear * gBear + wind * gWind + act * gAct + servo * gServo;
        mix = runBiquad(eqHigh_, runBiquad(eqCase_, runBiquad(eqLow_, mix)));
        out[i] = softClip(mix * p_.master);
    }
    publish();
}

DRIVE_RAM void DriveEngine::render(int16_t* out, size_t frames) {
    float buf[128];
    size_t done = 0;
    while (done < frames) {
        const size_t n = frames - done < 128 ? frames - done : 128;
        renderFloat(buf, n);
        for (size_t i = 0; i < n; ++i) out[done + i] = static_cast<int16_t>(buf[i] * 32767.0f);  // |buf| <= 1
        done += n;
    }
}

DRIVE_RAM void DriveEngine::publish() {
    const bool parked = !parkPending_;
    telState_.store(static_cast<uint8_t>(state_), std::memory_order_release);
    telRpm_.store(static_cast<uint32_t>(rpm_), std::memory_order_relaxed);
    telBusy_.store(busy_, std::memory_order_relaxed);
    telSeeks_.store(seekCount_, std::memory_order_relaxed);
    telLastSeekUs_.store(lastSeekUs_, std::memory_order_relaxed);
    const float xs = stepper() ? static_cast<float>(track_) / (cylinders() - 1) : clampf(x_, 0, 1);
    telCylinder_.store(static_cast<uint32_t>(lroundf(xs * (cylinders() - 1))), std::memory_order_relaxed);
    telStopped_.store(!powered_ && rpm_ < 0.02f * p_.rpm && parked, std::memory_order_release);
}
