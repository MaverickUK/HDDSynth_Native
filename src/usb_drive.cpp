#include "usb_drive.h"

#include <Arduino.h>
#include <hardware/structs/watchdog.h>
#include <USB.h>
#include <pico/mutex.h>
#include <class/msc/msc.h>
#include <device/usbd.h>
#include <tusb-msc.h>
#include <string.h>

#include "config.h"
#include "console.h"
#include "io.h"
#include "sd_streamer.h"
#include "version.h"

// TinyUSB (Arduino-Pico's USB stack) has a mass-storage class driver; we
// register its interface and answer its callbacks, as Arduino-Pico's own
// FatFSUSB library does for internal flash, but with the SD card's raw
// sectors. The host asks for 64-byte slices of 512-byte sectors (the USB
// endpoint size), so reads come from a small read-ahead cache and writes
// are gathered into whole sectors.

namespace usbdrive {

namespace {

constexpr uint32_t SECTOR = 512;
constexpr uint32_t READ_AHEAD = 8;  // sectors per SD read: 4 KB

uint32_t sectors = 0;
volatile bool shared = false;
volatile bool ejected = false;

uint8_t* readCache = nullptr;  // READ_AHEAD sectors, allocated only in this mode
uint32_t readFirst = 0, readCount = 0;
uint8_t writeBuf[SECTOR];
int64_t writeLba = -1;  // sector being gathered, or -1

void flushWrite() {
    if (writeLba >= 0) {
        sd::writeSectors(static_cast<uint32_t>(writeLba), writeBuf, 1);
        writeLba = -1;
    }
}

} // namespace

// Watchdog scratch 0: the SDK and boot ROM use 4-7 (reboot magic), and
// nothing here uses 0-3.
constexpr uint32_t DRIVE_MAGIC = 0x55534244;  // "USBD"

void requestAtNextBoot() { watchdog_hw->scratch[0] = DRIVE_MAGIC; }

bool requested() {
    if (watchdog_hw->scratch[0] == DRIVE_MAGIC) {
        watchdog_hw->scratch[0] = 0;  // once: the next reboot is a normal one
        return true;
    }
    pinMode(cfg::PIN_ACTION_BUTTON, INPUT_PULLUP);
    for (int i = 0; i < 30; ++i) {  // held for 300 ms, not a bounce
        if (digitalRead(cfg::PIN_ACTION_BUTTON) != LOW) return false;
        delay(10);
    }
    return true;
}

void run() {
    sectors = sd::cardSectors();
    readCache = new uint8_t[READ_AHEAD * SECTOR];
    io::ampOff();
    io::rgb(0, 0, 255);  // solid blue while shared
    Console.printf("[usb] SD card shared over USB (%lu MB). Eject it on the computer to go back to normal.\n",
                   static_cast<unsigned long>(sectors / 2048));

    // Re-enumerate with a mass-storage interface added to the serial port.
    USB.disconnect();
    const uint8_t epIn = USB.registerEndpointIn();
    const uint8_t epOut = USB.registerEndpointOut();
    static uint8_t desc[] = {TUD_MSC_DESCRIPTOR(1 /* fixed up by USB */, 0, epOut, epIn, CFG_TUD_MSC_EP_BUFSIZE)};
    USB.registerInterface(1, USBClass::simpleInterface, desc, sizeof(desc), 2, 0);
    USB.connect();
    shared = true;

    // Leave on eject, or on a press of the action button (released first).
    bool buttonWasUp = digitalRead(cfg::PIN_ACTION_BUTTON) == HIGH;
    for (;;) {
        const bool up = digitalRead(cfg::PIN_ACTION_BUTTON) == HIGH;
        if (ejected || (buttonWasUp && !up)) break;
        buttonWasUp = up;
        // Arduino-Pico runs the USB task from a 1 ms timer, which moves one
        // 64-byte slice per tick (~64 KB/s). Nothing else needs the CPU in
        // this mode, so run it continuously, under the same lock the timer
        // uses (if it's busy, the timer's own run is under way).
        if (mutex_try_enter(&USB.mutex, nullptr)) {
            tud_task();
            mutex_exit(&USB.mutex);
        }
    }
    shared = false;
    flushWrite();
    sd::finishTransfers();  // the card stays powered through the reboot: leave it idle
    Console.println(ejected ? "[usb] ejected, rebooting" : "[usb] button pressed, rebooting");
    Serial.flush();
    delay(300);  // let the host see the last command complete
    rp2040.reboot();
    for (;;) {}
}

// ---- TinyUSB mass-storage callbacks (run from the USB task on core 0) ----

int32_t read10(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!shared || lba >= sectors || offset + bufsize > SECTOR) return -1;
    if (writeLba >= 0) flushWrite();  // never read past a half-written sector
    if (lba < readFirst || lba >= readFirst + readCount) {
        uint32_t n = READ_AHEAD;
        if (lba + n > sectors) n = sectors - lba;
        if (!sd::readSectors(lba, readCache, n)) {
            readCount = 0;
            return -1;
        }
        readFirst = lba;
        readCount = n;
    }
    memcpy(buffer, readCache + (lba - readFirst) * SECTOR + offset, bufsize);
    return static_cast<int32_t>(bufsize);
}

int32_t write10(uint32_t lba, uint32_t offset, const uint8_t* buffer, uint32_t bufsize) {
    if (!shared || lba >= sectors || offset + bufsize > SECTOR) return -1;
    if (lba >= readFirst && lba < readFirst + readCount) readCount = 0;  // cached copy is now stale
    if (writeLba != static_cast<int64_t>(lba)) {
        flushWrite();
        if (offset != 0 && !sd::readSectors(lba, writeBuf, 1)) return -1;  // partial sector: keep the rest
        writeLba = lba;
    }
    memcpy(writeBuf + offset, buffer, bufsize);
    if (offset + bufsize == SECTOR) flushWrite();
    return static_cast<int32_t>(bufsize);
}

} // namespace usbdrive

extern "C" {

uint8_t tud_msc_get_maxlun_cb(void) { return 1; }

void tud_msc_inquiry_cb(uint8_t, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    memcpy(vendor_id, "HDDSynth", 8);
    memcpy(product_id, "SD card         ", 16);
    memcpy(product_rev, FIRMWARE_VERSION "    ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t) { return usbdrive::shared && !usbdrive::ejected; }

void tud_msc_capacity_cb(uint8_t, uint32_t* block_count, uint16_t* block_size) {
    *block_count = usbdrive::sectors;
    *block_size = usbdrive::SECTOR;
}

bool tud_msc_is_writable_cb(uint8_t) { return true; }


int32_t tud_msc_read10_cb(uint8_t, uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    return usbdrive::read10(lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    return usbdrive::write10(lba, offset, buffer, bufsize);
}

bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool start, bool load_eject) {
    if (!start && load_eject) usbdrive::ejected = true;  // "Eject" on the computer
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void*, uint16_t) {
    switch (scsi_cmd[0]) {
        case 0x1E:  // PREVENT/ALLOW MEDIUM REMOVAL: nothing to do
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);  // invalid command
            return -1;
    }
}

} // extern "C"
