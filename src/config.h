// HDD Synth MK5 firmware: board pins and build-time constants.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace cfg {

// ---- Pins (match hardware/HDD Synth Prototype MK5 and the CircuitPython settings.py) ----
constexpr uint8_t PIN_SAFETY        = 0;   // Reserved for XIP_CS1 / boot safety; unused here
constexpr uint8_t PIN_RGB_RED       = 4;   // Common-anode RGB LED, LOW = on
constexpr uint8_t PIN_RGB_GREEN     = 5;
constexpr uint8_t PIN_RGB_BLUE      = 6;
constexpr uint8_t PIN_SD_MISO       = 8;   // SPI1 RX
constexpr uint8_t PIN_SD_CS         = 9;
constexpr uint8_t PIN_SD_SCK        = 10;  // SPI1 SCK
constexpr uint8_t PIN_SD_MOSI       = 11;  // SPI1 TX
constexpr uint8_t PIN_I2S_DATA      = 12;
constexpr uint8_t PIN_I2S_BCLK      = 13;  // LRCLK is BCLK + 1 = GP14
constexpr uint8_t PIN_AMP_SDMODE    = 15;  // HIGH = amplifier on (left channel)
constexpr uint8_t PIN_HDD_LED       = 17;
constexpr uint8_t PIN_ACTIVITY      = 18;  // Optocoupler, LOW = HDD active
constexpr uint8_t PIN_ACTION_BUTTON = 19;
constexpr uint8_t PIN_ENC_A         = 20;
constexpr uint8_t PIN_ENC_B         = 21;
constexpr uint8_t PIN_ENC_BUTTON    = 22;
constexpr uint8_t PIN_SUPERCAP_ADC  = 27;  // ADC1, via 22k/22k divider
constexpr uint8_t PIN_POWER_FLT     = 28;  // eFuse /FLT, open drain, external pull-up. LOW = fault

// ---- Audio ----
constexpr uint32_t SAMPLE_RATE      = 16000;
constexpr size_t   MIX_BLOCK_FRAMES = 128;   // 8 ms per mix block
constexpr size_t   I2S_BUFFERS      = 6;     // 6 x 8 ms = 48 ms output latency
constexpr size_t   RING_SAMPLES     = 16384; // ~1.02 s of audio per streamed voice (32 KB)
constexpr size_t   SD_CHUNK_SAMPLES = 1024;  // 2 KB per SD read

// ---- SD ----
constexpr uint32_t SD_SPI_HZ        = 25000000;
constexpr int      SD_READ_RETRIES  = 3;
constexpr uint32_t SD_REMOUNT_INTERVAL_MS = 500;    // while the card is down, look for it this often
constexpr uint32_t SD_PROBE_HZ      = 400000;       // card init speed, used for the presence probe

// ---- Power ----
constexpr float    SUPERCAP_DIVIDER     = 0.5f;   // R24/R25 22k/22k
constexpr float    ADC_REF_V            = 3.3f;
constexpr uint32_t POWER_LOSS_CONFIRM_MS = 3;     // FLT must stay low this long
constexpr float    RESTORE_RISE_V       = 0.05f;
constexpr uint32_t RESTORE_WINDOW_MS    = 1000;

// ---- Safety ----
constexpr uint32_t WATCHDOG_MS        = 2000;
constexpr uint32_t CORE1_STALL_MS     = 500;   // core1 heartbeat must advance within this
constexpr size_t   HEAP_RESERVE_BYTES = 32 * 1024;

} // namespace cfg
