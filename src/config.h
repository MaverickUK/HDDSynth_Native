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
constexpr uint8_t PIN_OLED_SDA      = 24;  // J6 pin 4 (I2C0)
constexpr uint8_t PIN_OLED_SCL      = 25;  // J6 pin 3
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

// ---- Controls ----
constexpr int      VOLUME_STEP_PERCENT  = 5;     // per encoder detent (VOLUME_STEP 0.05 in settings.py)
constexpr int      BALANCE_STEP_PERCENT = 10;    // per encoder detent (BALANCE_STEP 0.1)
constexpr bool     ENCODER_REVERSED     = false; // flip if clockwise turns the volume down
constexpr uint32_t BUTTON_DEBOUNCE_MS   = 20;
constexpr uint32_t ACTION_SHORT_PRESS_MAX_MS = 1000;  // ACTION_BUTTON_SHORT_PRESS_MAX_S
constexpr uint32_t ACTION_LONG_PRESS_MS      = 3000;  // ACTION_BUTTON_LONG_PRESS_S

// ---- Jingle light show ----
constexpr uint32_t JINGLE_RAINBOW_MS   = 1500;  // one trip round the colour wheel
constexpr uint32_t JINGLE_LIGHT_STEP_MS = 10;   // LED update interval

// ---- Saved volume/balance ----
constexpr uint32_t SAVE_LEVELS_DEBOUNCE_MS = 2000;  // save this long after the last change

// ---- OLED (SSD1306 128x64 on J6) ----
constexpr uint8_t  OLED_I2C_ADDR       = 0x3C;
constexpr uint32_t OLED_I2C_HZ         = 400000;  // a full frame takes ~25 ms at this speed
constexpr uint32_t OLED_OVERLAY_MS     = 4000;    // volume/balance screen stays this long after a change
constexpr uint32_t OLED_FRAME_MS       = 100;     // at most 10 frames/s (each blocks core 0 ~25 ms)
constexpr uint32_t OLED_DEBUG_FRAME_MS = 250;     // debug text redraws at most 4 times/s
constexpr int      OLED_SUPERCAP_FULL_MV = 4200;  // top of the supercap graph

// ---- Demo mode ----
constexpr uint32_t DEMO_PACK_MS = 60000;  // how long each pack plays before spinning down

// ---- Safety ----
constexpr uint32_t WATCHDOG_MS        = 2000;
constexpr uint32_t CORE1_STALL_MS     = 500;   // core1 heartbeat must advance within this
constexpr size_t   HEAP_RESERVE_BYTES = 32 * 1024;

} // namespace cfg
