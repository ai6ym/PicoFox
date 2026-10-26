/* PicoFox - 2M FM Fox Transmitter

Copyright 2025 Giorgi Enterprises LLC dba AI6YM.radio.
License: Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (see LICENSE file).

COMMERCIAL USE OF THIS CODE AND ANY DERIVATIVE IS FORBIDDEN WITHOUT PERMISSION FROM THE COPYRIGHT HOLDER!

You may configure this software without compiling and flashing. Simply connect the device to your PC with a USB cable and edit the
settings.txt file. You may also change the audio which is played between morse code IDs by replacing the audio.wav file. The file you
provide MUST be 5kHz sampling, unsigned 16bit PCM audio.

The device may be reset to factory settings by deleting the settings.txt and audio.wav files. In factory settings the device WILL NOT
transmit. Configuring a callsign is required to enable the transmitter.
*/

#include <Arduino.h>
#include <Wire.h>
#include <hardware/clocks.h>
#include <hardware/flash.h>
#include <hardware/regs/clocks.h>
#include <hardware/regs/m0plus.h>
#include <hardware/structs/clocks.h>
#include <hardware/structs/scb.h>
#include <hardware/sync.h>
#include <si5351.h>
#include "SPI.h"
#include "SdFat_Adafruit_Fork.h"
#include "Adafruit_SPIFlash.h"
#include "Adafruit_TinyUSB.h"

#include "audio.h"

// Generate/validate the carrier-specific LUT at full speed, then reduce the
// system clock before starting the real-time playback path.
#define STARTUP_CPU_CLOCK_HZ 200000000UL
#define PLAYBACK_CPU_CLOCK_KHZ 60000UL

#if F_CPU != STARTUP_CPU_CLOCK_HZ
#error "PicoFox must be built for a 200MHz startup clock"
#endif

// Board revision ID pins.
// Pulldown required. 0000 == r2, 1000 == r3.
#define ID0 22u
#define ID1 23u
#define ID2 24u
#define ID3 25u

#define SDA 0u                                  // I2C data pin.
#define SCL 1u                                  // I2C clock pin.
#define I2C_CLOCK_HZ 1000000                    // >= 800kHz needed for sufficiently fast I2C writes.
#define AMP_EN 2u                               // Pin which enables the amplifier. Set LOW to enable, HIGH to disable.
#define TX_LED 3u                               // Transmit indicator LED. Set HIGH while transmitting.
#define ATTN_LE 4u                              // Pin which latches the attenuator.
#define AUDIO_SAMPLE_RATE_HZ 5000               // Audio sample rate (also rate of updates to SI5351).
#define FM_DEVIATION_HZ 5000                    // Maximum peak RF deviation in Hz for conventional amateur FM.
#define SI5351_PLL SI5351_PLLA                  // PLL which will be used on the SI5351, consider this PLL unavailable for other purposes.
#define SI5351_CLOCK_OUTPUT SI5351_CLK0         // Clock output which drives RF circuitry.
#define SI5351_REFERENCE_HZ 25000000ULL         // External TCXO frequency.
#define SI5351_PLL_MAX_DENOM 1048575U           // 2^20 - 1, max denominator for the fractional PLL feedback divider.
#define SI5351_PLL_PARAM_START SI5351_PLLA_PARAMETERS  // Register address on which PLLA parameters start.
#define SI5351_CLOCK_DIV 6                      // Fixed divider between PLL clock and MS.
#define SI5351_DRIVE_LEVEL_R2 SI5351_DRIVE_2MA  // Power output from SI5351, higher levels don't add much output but do create spurious emmissions.
#define SI5351_DRIVE_LEVEL_R3 SI5351_DRIVE_4MA  // Power output from SI5351.
#define SI5351_LOW_DRIVE_ATTENUATION_STEPS 24   // 6dB at 0.25dB/step, matching the measured 4mA-to-2mA output reduction.
#define ATTENUATOR_MAX_STEPS 127                // BVA1761 hardware limit: 31.75dB at 0.25dB/step.
#define CONFIG_MAX_ATTENUATION_STEPS 151        // 37.75dB total: 6dB from the SI5351 plus 31.75dB from the BVA1761.
#define AUDIO_FADE_SAMPLES (AUDIO_SAMPLE_RATE_HZ / 100)  // 10ms fade around runtime loop and trim boundaries.
#define MAX_SCHEDULE_SECS UINT32_MAX
#define MAX_CYCLE_ON_SECS (UINT32_MAX / AUDIO_SAMPLE_RATE_HZ)
#define MAX_CYCLE_OFF_SECS (UINT32_MAX / 1000UL)
#define SAMPLE_WRITE_CORR_US -3                 // Time correction between sample writes to account for code which can't be measured.
#define MAX_UINT32 4294967295                   // Maximum value for a 32 bit unsigned integer. Used to prevent glitches when micros() overflows.
#define PLL_WRITE_BUFFER_CAPACITY 256            // Exact resolved PLL writes buffered in SRAM; must be a power of two.
#define PLL_WRITE_BUFFER_REFILL_THRESHOLD (PLL_WRITE_BUFFER_CAPACITY / 2)  // Refill in batches to reduce core wakeups.

#define TEST_MODE_PIN 14u                       // Pin which is monitored to enter test mode. If you use this pin feel free to remove this logic.

static_assert(MAX_SCHEDULE_SECS >= 86400UL, "Schedule settings must support at least one day");
static_assert(MAX_CYCLE_ON_SECS >= 86400UL, "CYCLE_ON_SECS must support at least one day");
static_assert(MAX_CYCLE_OFF_SECS >= 86400UL, "CYCLE_OFF_SECS must support at least one day");

// Frequency limitations.
#define MIN_FREQ_MHZ 144
#define MAX_FREQ_MHZ_ITU1 146  // ITU zone 1.
#define MAX_FREQ_MHZ 148       // ITU zones 2 and 3.

// Test mode detected. Indicates program / test before shipping.
bool test_mode;

// Board revision.
uint16_t revision;
bool wireRunning = false;

// RF Clock Gen
Si5351 si5351;

// Uses same external flash chip that the RP2040 is executing from. Size depends on Tools -> Flash Size setting.
Adafruit_FlashTransport_RP2040 flashTransport;
Adafruit_SPIFlash flash(&flashTransport);

// Filesystem and file descriptors.
FatFormatter formatter;
FatVolume fatfs;
FatFile root;
FatFile file;
uint8_t filesystemFormatBuffer[4096];
uint8_t filesystemSectorBuffer[512];

struct VerifiedFileWrite {
  uint32_t size;
  uint32_t hash;
};

// Controller for USB mass storage device.
Adafruit_USBD_MSC usb_msc;

// Flash file names.
const char SETTINGS_TXT[] = "settings.txt";
const char AUDIO_WAV[] = "audio.wav";
const char CALLSIGN_WAV[] = ".callsign.wav";
const char SETTINGS_CRC[] = ".settings_crc.bin";
const char BOOTSEL_TRIGGER[] = "BOOTSEL";

// Default settings.
const char DEFAULT_CALLSIGN[12] = "";
const uint8_t DEFAULT_ITU_ZONE = 2;
const double DEFAULT_FREQ_MHZ = 146.565;
const uint8_t DEFAULT_DUTY_CYCLE = 100;
const uint32_t DEFAULT_INITIAL_SLEEP_SECS = 0;
const uint32_t DEFAULT_RUNTIME_SECS = 0;  // Zero keeps transmitting indefinitely.
const uint32_t DEFAULT_CYCLE_ON_SECS = 60;
const uint32_t DEFAULT_CYCLE_OFF_SECS = 60;
const uint8_t DEFAULT_WPM = 15;
const uint8_t DEFAULT_FARNSWORTH_WPM = 10;
const uint16_t DEFAULT_MORSE_TONE_HZ = 600;
const uint8_t DEFAULT_TONE_AMPLITUDE_PERCENT = 70;
const uint8_t DEFAULT_ATTENUATION = 0;

// Config struct, start by loading defaults.
struct Settings {
  char callsign[12];
  uint8_t ituZone;
  double transmitFreqMHz;
  uint8_t dutyCyclePercent;
  uint32_t initialSleepSeconds;
  uint32_t runtimeSeconds;
  uint32_t cycleOnSeconds;
  uint32_t cycleOffSeconds;
  uint8_t morseWPM;
  uint8_t farnsworthWPM;
  uint16_t morseToneHz;
  uint8_t toneAmplitudePercent;
  bool isConfigured;
  uint8_t attenuation;
};

Settings settings = {
  .callsign = { *DEFAULT_CALLSIGN },
  .ituZone = DEFAULT_ITU_ZONE,
  .transmitFreqMHz = DEFAULT_FREQ_MHZ,
  .dutyCyclePercent = DEFAULT_DUTY_CYCLE,
  .initialSleepSeconds = DEFAULT_INITIAL_SLEEP_SECS,
  .runtimeSeconds = DEFAULT_RUNTIME_SECS,
  .cycleOnSeconds = DEFAULT_CYCLE_ON_SECS,
  .cycleOffSeconds = DEFAULT_CYCLE_OFF_SECS,
  .morseWPM = DEFAULT_WPM,
  .farnsworthWPM = DEFAULT_FARNSWORTH_WPM,
  .morseToneHz = DEFAULT_MORSE_TONE_HZ,
  .isConfigured = (strlen(DEFAULT_CALLSIGN) > 0),
  .attenuation = DEFAULT_ATTENUATION
};

int settingsCrc = 0;

// SI5351 lookup table stored in the firmware partition. The table is read directly through XIP at playback time.
constexpr uint32_t SI5351_LUT_MAGIC = 0x3154554c;  // "LUT1" in little-endian order.
constexpr uint16_t SI5351_LUT_VERSION = 1;
constexpr uint32_t SI5351_LUT_ENTRY_COUNT = 65536;
constexpr uint32_t SI5351_LUT_ENTRY_SIZE = 8;

struct Si5351LutHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t entrySize;
  uint32_t entryCount;
  uint64_t carrierCentiHz;
  uint32_t referenceHz;
  uint32_t deviationHz;
  uint32_t clockDiv;
  uint32_t maxDenominator;
  uint32_t dataCrc32;
};

struct RationalFraction {
  uint32_t numerator;
  uint32_t denominator;
};

struct alignas(FLASH_SECTOR_SIZE) Si5351LutStorage {
  uint8_t headerSector[FLASH_SECTOR_SIZE];
  uint8_t entries[SI5351_LUT_ENTRY_COUNT][SI5351_LUT_ENTRY_SIZE];
};

const volatile Si5351LutStorage si5351LutStorage
  __attribute__((used, section(".flashdata.si5351_lut"))) = {};

static_assert((sizeof(Si5351LutStorage) % FLASH_SECTOR_SIZE) == 0, "SI5351 LUT must occupy complete flash sectors");

alignas(4) uint8_t si5351LutSectorBuffer[FLASH_SECTOR_SIZE];
bool frequencyLutReady = false;
uint32_t callsignAudioSamples = 0;
volatile bool runtimeComplete = false;
volatile bool runtimeEnding = false;
volatile bool normalProducerStopped = false;
volatile bool finalQueueReady = false;
bool queuingFinalId = false;
uint64_t runtimeDeadlineUs = 0;

enum BufferedCommandType : uint8_t {
  BUFFERED_PLL_WRITE,
  BUFFERED_OUTPUT_ON,
  BUFFERED_OUTPUT_OFF,
  BUFFERED_DELAY_MS,
  BUFFERED_FINISH_RUNTIME
};

struct BufferedCommand {
  BufferedCommandType type;
  uint8_t pllParams[SI5351_LUT_ENTRY_SIZE];
  uint32_t delayMs;
};

static_assert((PLL_WRITE_BUFFER_CAPACITY & (PLL_WRITE_BUFFER_CAPACITY - 1)) == 0,
              "PLL write buffer capacity must be a power of two");

BufferedCommand pllWriteBuffer[PLL_WRITE_BUFFER_CAPACITY];
volatile uint32_t pllWriteBufferHead = 0;
volatile uint32_t pllWriteBufferTail = 0;
volatile bool pllWriteStreamStarted = false;

uint32_t pllWriteBufferDepth() {
  return pllWriteBufferHead - pllWriteBufferTail;
}

bool tryEnqueueBufferedCommand(const BufferedCommand& command) {
  uint32_t head = pllWriteBufferHead;
  uint32_t previousDepth = head - pllWriteBufferTail;
  if (previousDepth >= PLL_WRITE_BUFFER_CAPACITY) {
    return false;
  }

  pllWriteBuffer[head & (PLL_WRITE_BUFFER_CAPACITY - 1)] = command;
  __dmb();
  pllWriteBufferHead = head + 1;
  pllWriteStreamStarted = true;

  uint32_t depth = head + 1 - pllWriteBufferTail;
  if (previousDepth == 0 || depth == PLL_WRITE_BUFFER_REFILL_THRESHOLD) {
    __sev();
  }
  return true;
}

bool enqueueBufferedCommand(const BufferedCommand& command) {
  if (runtimeEnding && !queuingFinalId) return false;
  while (!tryEnqueueBufferedCommand(command)) {
    if (runtimeEnding && !queuingFinalId) return false;
    __wfe();
  }
  return true;
}

bool tryDequeueBufferedCommand(BufferedCommand& command) {
  uint32_t tail = pllWriteBufferTail;
  if (tail == pllWriteBufferHead) {
    return false;
  }

  __dmb();
  command = pllWriteBuffer[tail & (PLL_WRITE_BUFFER_CAPACITY - 1)];
  __dmb();
  pllWriteBufferTail = tail + 1;
  if (pllWriteBufferHead - (tail + 1) == PLL_WRITE_BUFFER_REFILL_THRESHOLD) {
    __sev();
  }
  return true;
}

// Disable clock endpoints which PicoFox never uses, and retain only the timer
// and USB controller clocks when both processors are asleep. The USB PLL,
// clk_usb and PHY remain running so a host can connect at any time.
void configureRp2040PowerSaving() {
  constexpr uint32_t unusedWakeClocks0 =
    CLOCKS_WAKE_EN0_CLK_SYS_SPI1_BITS |
    CLOCKS_WAKE_EN0_CLK_PERI_SPI1_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_SPI0_BITS |
    CLOCKS_WAKE_EN0_CLK_PERI_SPI0_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_RTC_BITS |
    CLOCKS_WAKE_EN0_CLK_RTC_RTC_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_PWM_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_PIO1_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_PIO0_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_JTAG_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_I2C1_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_DMA_BITS |
    CLOCKS_WAKE_EN0_CLK_SYS_ADC_BITS |
    CLOCKS_WAKE_EN0_CLK_ADC_ADC_BITS;
  constexpr uint32_t unusedWakeClocks1 =
    CLOCKS_WAKE_EN1_CLK_SYS_UART1_BITS |
    CLOCKS_WAKE_EN1_CLK_PERI_UART1_BITS |
    CLOCKS_WAKE_EN1_CLK_SYS_UART0_BITS |
    CLOCKS_WAKE_EN1_CLK_PERI_UART0_BITS |
    CLOCKS_WAKE_EN1_CLK_SYS_TBMAN_BITS;

  hw_clear_bits(&clocks_hw->wake_en0, unusedWakeClocks0);
  hw_clear_bits(&clocks_hw->wake_en1, unusedWakeClocks1);

  // These generators are unused even while awake. USB remains untouched.
  clock_stop(clk_adc);
  clock_stop(clk_rtc);

  clocks_hw->sleep_en0 = 0;
  clocks_hw->sleep_en1 =
    CLOCKS_SLEEP_EN1_CLK_SYS_TIMER_BITS |
    CLOCKS_SLEEP_EN1_CLK_SYS_USBCTRL_BITS |
    CLOCKS_SLEEP_EN1_CLK_USB_USBCTRL_BITS;
}

void enableProcessorDeepSleep() {
  scb_hw->scr |= M0PLUS_SCR_SLEEPDEEP_BITS;
}

// sleep_ms() uses the RP2040 timer alarm and WFE path. Chunking avoids the
// 32-bit millisecond limit while allowing schedule values up to UINT32_MAX.
void sleepForSeconds(uint32_t seconds) {
  uint64_t remainingMs = (uint64_t)seconds * 1000ULL;
  while (remainingMs > 0) {
    uint32_t chunkMs = remainingMs > UINT32_MAX ? UINT32_MAX : (uint32_t)remainingMs;
    sleep_ms(chunkMs);
    remainingMs -= chunkMs;
  }
}

void sleepForever() {
  while (true) __wfe();
}

void setPlaybackClockSpeed() {
  // LUT validation/generation and all filesystem startup work run at the
  // build's 200MHz clock. Playback needs only 60MHz, so reduce clk_sys after
  // that work is complete and before configuring the I2C peripheral.
  set_sys_clock_khz(PLAYBACK_CPU_CLOCK_KHZ, true);
}

bool labelFormattedFat16Volume() {
  if (!flash.readSector(0, filesystemSectorBuffer)) return false;
  MbrSector_t* mbr = reinterpret_cast<MbrSector_t*>(filesystemSectorBuffer);
  if (getLe16(mbr->signature) != MBR_SIGNATURE) return false;
  uint32_t partitionStart = getLe32(mbr->part[0].relativeSectors);

  if (!flash.readSector(partitionStart, filesystemSectorBuffer)) return false;
  PbsFat_t* pbs = reinterpret_cast<PbsFat_t*>(filesystemSectorBuffer);
  if (getLe16(pbs->signature) != PBR_SIGNATURE) return false;
  uint16_t reservedSectors = getLe16(pbs->bpb.bpb16.reservedSectorCount);
  uint16_t fatSectors = getLe16(pbs->bpb.bpb16.sectorsPerFat16);
  uint32_t rootStart = partitionStart + reservedSectors +
                       pbs->bpb.bpb16.fatCount * fatSectors;
  memcpy(pbs->bpb.bpb16.volumeLabel, "PICOFOX    ", 11);
  if (!flash.writeSector(partitionStart, filesystemSectorBuffer)) return false;

  if (!flash.readSector(rootStart, filesystemSectorBuffer)) return false;
  memset(filesystemSectorBuffer, 0, 32);
  memcpy(filesystemSectorBuffer, "PICOFOX    ", 11);
  filesystemSectorBuffer[11] = 0x08;
  if (!flash.writeSector(rootStart, filesystemSectorBuffer)) return false;
  if (!flash.syncBlocks()) return false;

  if (!flash.readSector(partitionStart, filesystemSectorBuffer)) return false;
  pbs = reinterpret_cast<PbsFat_t*>(filesystemSectorBuffer);
  if (memcmp(pbs->bpb.bpb16.volumeLabel, "PICOFOX    ", 11) != 0) return false;
  if (!flash.readSector(rootStart, filesystemSectorBuffer)) return false;
  return memcmp(filesystemSectorBuffer, "PICOFOX    ", 11) == 0 &&
         filesystemSectorBuffer[11] == 0x08;
}

// Format the complete 15MB user partition as FAT16. SdFat deliberately refuses
// media of 6MB or less, but the production PicoFox layout is large enough for
// its standard FAT16 formatter. Add and read back the product volume label
// before mounting the new filesystem.
bool formatFlashFat16() {
  fatfs.end();
  if (!formatter.format(&flash, filesystemFormatBuffer)) return false;
  if (!labelFormattedFat16Volume()) return false;
  return fatfs.begin(&flash, true, 1, 0);
}

// SdFat can mount a damaged boot sector even when it claims more sectors than
// the physical flash contains. Reject impossible geometry and unreadable FAT
// metadata so partially mountable corruption also enters the recovery path.
bool flashFilesystemSane() {
  if (fatfs.fatType() != 16 || fatfs.clusterCount() == 0 ||
      fatfs.rootDirEntryCount() == 0) {
    return false;
  }

  uint32_t fatStart = fatfs.fatStartSector();
  uint32_t rootStart = fatfs.rootDirStart();
  uint32_t dataStart = fatfs.dataStartSector();
  uint64_t dataEnd = (uint64_t)dataStart +
                     (uint64_t)fatfs.clusterCount() * fatfs.sectorsPerCluster();
  if (!(fatStart < rootStart && rootStart < dataStart) ||
      dataEnd > flash.sectorCount()) {
    return false;
  }

  FatFile checkRoot;
  bool rootReadable = checkRoot.openRoot(&fatfs);
  bool rootClosed = checkRoot.close();
  return rootReadable && rootClosed && fatfs.freeClusterCount() >= 0;
}

// Reads data from flash device.
int32_t mscReadCb(uint32_t lba, void* buffer, uint32_t bufsize) {
  return flash.readBlocks(lba, (uint8_t*)buffer, bufsize / 512) ? bufsize : -1;
}

// Writes data to the flash device.
int32_t mscWriteCb(uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
  return flash.writeBlocks(lba, buffer, bufsize / 512) ? bufsize : -1;
}

// Flushes writes to flash.
void mscFlushCb(void) {
  flash.syncBlocks();
}

bool openRoot() {
  return root.openRoot(&fatfs);
}

bool closeRoot() {
  return root.close();
}

constexpr uint32_t FILE_HASH_OFFSET = 2166136261UL;
constexpr uint32_t FILE_HASH_PRIME = 16777619UL;

uint32_t updateFileHash(uint32_t hash, const void* data, size_t length) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; i++) {
    hash ^= bytes[i];
    hash *= FILE_HASH_PRIME;
  }
  return hash;
}

bool fileExistsChecked(const char* filename, bool* exists) {
  if (!openRoot()) return false;
  root.clearError();
  *exists = root.exists(filename);
  bool success = root.getError() == 0;
  bool closed = closeRoot();
  return success && closed;
}

bool readFileDigest(const char* filename, uint32_t* sizeOut, uint32_t* hashOut) {
  if (!openRoot()) return false;
  if (!file.open(&root, filename, O_RDONLY)) {
    closeRoot();
    return false;
  }

  uint32_t expectedSize = file.fileSize();
  uint32_t remaining = expectedSize;
  uint32_t hash = FILE_HASH_OFFSET;
  uint8_t buffer[256];
  bool success = true;
  while (remaining > 0) {
    size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    int count = file.read(buffer, requested);
    if (count <= 0) {
      success = false;
      break;
    }
    hash = updateFileHash(hash, buffer, count);
    remaining -= count;
  }
  if (file.getError()) success = false;

  bool fileClosed = file.close();
  bool rootClosed = closeRoot();
  if (!success || remaining != 0 || !fileClosed || !rootClosed) return false;

  *sizeOut = expectedSize;
  *hashOut = hash;
  return true;
}

bool verifyFileDigest(const char* filename, uint32_t expectedSize, uint32_t expectedHash) {
  uint32_t actualSize;
  uint32_t actualHash;
  return readFileDigest(filename, &actualSize, &actualHash) &&
         actualSize == expectedSize && actualHash == expectedHash;
}

bool beginVerifiedFileWrite(const char* filename, VerifiedFileWrite* state) {
  if (!openRoot()) return false;
  if (!file.open(&root, filename, O_RDWR | O_CREAT)) {
    closeRoot();
    return false;
  }
  if (!file.truncate(0)) {
    file.close();
    closeRoot();
    return false;
  }
  state->size = 0;
  state->hash = FILE_HASH_OFFSET;
  return true;
}

bool writeVerifiedChunk(VerifiedFileWrite* state, const void* data, size_t length) {
  if (length == 0) return true;
  if (file.write(data, length) != length || file.getWriteError()) return false;
  state->size += length;
  state->hash = updateFileHash(state->hash, data, length);
  return true;
}

void abandonVerifiedFileWrite() {
  if (file.isOpen()) file.close();
  if (root.isOpen()) closeRoot();
}

bool finishVerifiedFileWrite(const char* filename, const VerifiedFileWrite& state) {
  bool writeClean = !file.getWriteError();
  bool fileClosed = file.close();
  bool rootClosed = closeRoot();
  bool synced = flash.syncBlocks();
  if (!writeClean || !fileClosed || !rootClosed || !synced) return false;
  return verifyFileDigest(filename, state.size, state.hash);
}

bool writeVerifiedFile(const char* filename, const void* data, size_t length) {
  VerifiedFileWrite state;
  if (!beginVerifiedFileWrite(filename, &state)) return false;
  if (!writeVerifiedChunk(&state, data, length)) {
    abandonVerifiedFileWrite();
    return false;
  }
  return finishVerifiedFileWrite(filename, state);
}

bool readTextFile(const char* filename, String* contents) {
  if (!openRoot()) return false;
  if (!file.open(&root, filename, O_RDONLY)) {
    closeRoot();
    return false;
  }

  uint32_t expectedSize = file.fileSize();
  if (expectedSize > 4096 || !contents->reserve(expectedSize + 1)) {
    file.close();
    closeRoot();
    return false;
  }
  contents->remove(0);
  uint8_t buffer[128];
  uint32_t remaining = expectedSize;
  bool success = true;
  while (remaining > 0) {
    size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    int count = file.read(buffer, requested);
    if (count <= 0) {
      success = false;
      break;
    }
    for (int i = 0; i < count; i++) *contents += (char)buffer[i];
    remaining -= count;
  }
  if (file.getError()) success = false;

  bool fileClosed = file.close();
  bool rootClosed = closeRoot();
  return success && remaining == 0 && fileClosed && rootClosed;
}

// Enter the ROM USB bootloader when a BOOTSEL marker is found on the user
// volume. Delete and flush the marker first so the next application boot is
// normal, and remain in the application if deletion fails to avoid a loop.
bool checkBootselTrigger() {
  if (!openRoot()) return false;

  root.clearError();
  bool triggerPresent = root.exists(BOOTSEL_TRIGGER);
  if (root.getError()) {
    closeRoot();
    return false;
  }
  bool triggerRemoved = !triggerPresent || root.remove(BOOTSEL_TRIGGER);
  bool rootClosed = closeRoot();

  if (!triggerRemoved || !rootClosed) return false;
  if (!triggerPresent) return true;

  if (!flash.syncBlocks()) return false;
  delay(10);
  rp2040.rebootToBootloader();
  return true;
}

// Saves the default settings struct to the settings.txt file in flash.
bool saveDefaultSettings() {
  char buffer[512];
  int length = snprintf(
    buffer,
    sizeof(buffer),
    "CALLSIGN=%s\nITU_ZONE=%u\nFREQ_MHZ=%.6f\nDUTY_CYCLE=%u\nINITIAL_SLEEP_SECS=%lu\nRUNTIME_SECS=%lu\nCYCLE_ON_SECS=%lu\nCYCLE_OFF_SECS=%lu\nATTENUATION=%u\nMORSE_WPM=%u\nMORSE_FARNSWORTH_WPM=%u\nMORSE_TONE=%u\nMORSE_TONE_VOL=%u\n",
    DEFAULT_CALLSIGN,
    DEFAULT_ITU_ZONE,
    DEFAULT_FREQ_MHZ,
    DEFAULT_DUTY_CYCLE,
    (unsigned long)DEFAULT_INITIAL_SLEEP_SECS,
    (unsigned long)DEFAULT_RUNTIME_SECS,
    (unsigned long)DEFAULT_CYCLE_ON_SECS,
    (unsigned long)DEFAULT_CYCLE_OFF_SECS,
    DEFAULT_ATTENUATION,
    DEFAULT_WPM,
    DEFAULT_FARNSWORTH_WPM,
    DEFAULT_MORSE_TONE_HZ,
    DEFAULT_TONE_AMPLITUDE_PERCENT);
  return length > 0 && length < (int)sizeof(buffer) &&
         writeVerifiedFile(SETTINGS_TXT, buffer, length);
}

uint32_t parseSeconds(const String& value, uint32_t maximum) {
  String normalized = value;
  normalized.trim();
  if (normalized.length() == 0 || normalized[0] == '-') return 0;

  uint64_t parsed = 0;
  for (unsigned int i = 0; i < normalized.length(); i++) {
    char character = normalized[i];
    if (character < '0' || character > '9') break;
    parsed = parsed * 10 + (character - '0');
    if (parsed >= maximum) return maximum;
  }
  return (uint32_t)parsed;
}

void noteTimingSetting(
  const String& sourceLine,
  bool& foundInitialSleepSeconds,
  bool& foundRuntimeSeconds,
  bool& foundCycleOnSeconds,
  bool& foundCycleOffSeconds) {
  int separator = sourceLine.indexOf('=');
  if (separator < 0) return;

  String key = sourceLine.substring(0, separator);
  key.trim();
  key.toUpperCase();
  if (key == "INITIAL_SLEEP_SECS") foundInitialSleepSeconds = true;
  else if (key == "RUNTIME_SECS") foundRuntimeSeconds = true;
  else if (key == "CYCLE_ON_SECS") foundCycleOnSeconds = true;
  else if (key == "CYCLE_OFF_SECS") foundCycleOffSeconds = true;
}

// Adds the new timing keys to settings files created by older firmware while
// preserving every existing line and any user comments.
bool addMissingTimingSettings() {
  String contents;
  if (!readTextFile(SETTINGS_TXT, &contents)) return false;
  bool foundInitialSleepSeconds = false;
  bool foundRuntimeSeconds = false;
  bool foundCycleOnSeconds = false;
  bool foundCycleOffSeconds = false;
  bool endsWithNewline = true;
  String line;
  for (unsigned int i = 0; i < contents.length(); i++) {
    char character = contents[i];
    endsWithNewline = character == '\n';
    if (character == '\n') {
      noteTimingSetting(
        line,
        foundInitialSleepSeconds,
        foundRuntimeSeconds,
        foundCycleOnSeconds,
        foundCycleOffSeconds);
      line = "";
    } else {
      line += character;
    }
  }
  if (line.length() > 0) {
    noteTimingSetting(
      line,
      foundInitialSleepSeconds,
      foundRuntimeSeconds,
      foundCycleOnSeconds,
      foundCycleOffSeconds);
  }

  if (!foundInitialSleepSeconds || !foundRuntimeSeconds ||
      !foundCycleOnSeconds || !foundCycleOffSeconds) {
    if (!endsWithNewline && contents.length() > 0) contents += '\n';
    if (!foundInitialSleepSeconds) {
      contents += "INITIAL_SLEEP_SECS=";
      contents += String(DEFAULT_INITIAL_SLEEP_SECS);
      contents += '\n';
    }
    if (!foundRuntimeSeconds) {
      contents += "RUNTIME_SECS=";
      contents += String(DEFAULT_RUNTIME_SECS);
      contents += '\n';
    }
    if (!foundCycleOnSeconds) {
      contents += "CYCLE_ON_SECS=";
      contents += String(DEFAULT_CYCLE_ON_SECS);
      contents += '\n';
    }
    if (!foundCycleOffSeconds) {
      contents += "CYCLE_OFF_SECS=";
      contents += String(DEFAULT_CYCLE_OFF_SECS);
      contents += '\n';
    }
    return writeVerifiedFile(SETTINGS_TXT, contents.c_str(), contents.length());
  }
  return true;
}

// Rewrites one setting while preserving all unrelated lines. This is used to
// make an automatically corrected CYCLE_ON_SECS value visible on the USB drive.
bool writeSettingValue(const char* settingName, uint32_t settingValue) {
  String contents;
  if (!readTextFile(SETTINGS_TXT, &contents)) return false;

  String updated;
  updated.reserve(contents.length() + 32);
  bool replaced = false;
  int lineStart = 0;
  while (lineStart < (int)contents.length()) {
    int newline = contents.indexOf('\n', lineStart);
    bool hasNewline = newline >= 0;
    int lineEnd = hasNewline ? newline : contents.length();
    String line = contents.substring(lineStart, lineEnd);
    int separator = line.indexOf('=');
    String key = separator >= 0 ? line.substring(0, separator) : "";
    key.trim();
    key.toUpperCase();

    if (key == settingName) {
      updated += settingName;
      updated += '=';
      updated += String(settingValue);
      replaced = true;
    } else {
      updated += line;
    }
    if (hasNewline) updated += '\n';
    lineStart = lineEnd + (hasNewline ? 1 : 0);
  }

  if (!replaced) {
    if (updated.length() > 0 && !updated.endsWith("\n")) updated += '\n';
    updated += settingName;
    updated += '=';
    updated += String(settingValue);
    updated += '\n';
  }

  return writeVerifiedFile(SETTINGS_TXT, updated.c_str(), updated.length());
}

// Saves the default audio wav to the audio.wav file in flash.
bool saveDefaultAudio() {
  VerifiedFileWrite state;
  if (!beginVerifiedFileWrite(AUDIO_WAV, &state)) return false;
  bool success = writeVerifiedChunk(&state, wavHeader, sizeof(wavHeader));
  // Loop default audio to produce longer playback between IDs and transmitter cycling.
  for (int i = 0; success && i < DEFAULT_AUDIO_LOOPS; i++) {
    success = writeVerifiedChunk(&state, defaultAudio, sizeof(defaultAudio));
  }
  if (!success) {
    abandonVerifiedFileWrite();
    return false;
  }
  return finishVerifiedFileWrite(AUDIO_WAV, state);
}

bool wavSampleCount(const char* filename, uint32_t* sampleCountOut) {
  if (!openRoot()) return false;
  if (!file.open(&root, filename, O_RDONLY)) {
    closeRoot();
    return false;
  }

  uint8_t header[44];
  uint32_t fileSize = file.fileSize();
  bool success = fileSize >= sizeof(header) &&
                 ((fileSize - sizeof(header)) % sizeof(int16_t)) == 0 &&
                 file.read(header, sizeof(header)) == (int)sizeof(header) &&
                 memcmp(header, "RIFF", 4) == 0 &&
                 memcmp(header + 8, "WAVE", 4) == 0;

  uint8_t buffer[256];
  uint32_t remaining = success ? fileSize - sizeof(header) : 0;
  while (success && remaining > 0) {
    size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    int count = file.read(buffer, requested);
    if (count <= 0) {
      success = false;
      break;
    }
    remaining -= count;
  }
  if (file.getError()) success = false;
  bool fileClosed = file.close();
  bool rootClosed = closeRoot();
  if (!success || remaining != 0 || !fileClosed || !rootClosed) return false;

  *sampleCountOut = (fileSize - sizeof(header)) / sizeof(int16_t);
  return true;
}

// Loads the settings.txt file in flash into a settings struct.
bool loadSettings() {
  if (!openRoot()) return false;
  if (!file.open(&root, SETTINGS_TXT, O_RDONLY)) {
    closeRoot();
    return false;
  }

  memset(settings.callsign, 0, sizeof(settings.callsign));
  settings.ituZone = DEFAULT_ITU_ZONE;
  settings.transmitFreqMHz = DEFAULT_FREQ_MHZ;
  settings.dutyCyclePercent = DEFAULT_DUTY_CYCLE;
  settings.initialSleepSeconds = DEFAULT_INITIAL_SLEEP_SECS;
  settings.runtimeSeconds = DEFAULT_RUNTIME_SECS;
  settings.cycleOnSeconds = DEFAULT_CYCLE_ON_SECS;
  settings.cycleOffSeconds = DEFAULT_CYCLE_OFF_SECS;
  settings.morseWPM = DEFAULT_WPM;
  settings.farnsworthWPM = DEFAULT_FARNSWORTH_WPM;
  settings.morseToneHz = DEFAULT_MORSE_TONE_HZ;
  settings.toneAmplitudePercent = DEFAULT_TONE_AMPLITUDE_PERCENT;
  settings.attenuation = DEFAULT_ATTENUATION;
  settings.isConfigured = false;

  String line = "";
  int c;
  settingsCrc = 0;
  while ((c = file.read()) >= 0) {
    settingsCrc += c;
    char ch = (char)c;
    ch = toupper(ch);
    if (ch == '\n') {
      int sep = line.indexOf('=');
      if (sep == -1) {
        line = "";
        continue;
      }
      String key = line.substring(0, sep);
      String val = line.substring(sep + 1);
      if (key == "CALLSIGN") strncpy(settings.callsign, val.c_str(), sizeof(settings.callsign) - 1);
      else if (key == "ITU_ZONE") settings.ituZone = val.toInt();
      else if (key == "FREQ_MHZ") settings.transmitFreqMHz = val.toDouble();
      else if (key == "DUTY_CYCLE") settings.dutyCyclePercent = val.toInt();
      else if (key == "INITIAL_SLEEP_SECS") settings.initialSleepSeconds = parseSeconds(val, MAX_SCHEDULE_SECS);
      else if (key == "RUNTIME_SECS") settings.runtimeSeconds = parseSeconds(val, MAX_SCHEDULE_SECS);
      else if (key == "CYCLE_ON_SECS") settings.cycleOnSeconds = parseSeconds(val, MAX_CYCLE_ON_SECS);
      else if (key == "CYCLE_OFF_SECS") settings.cycleOffSeconds = parseSeconds(val, MAX_CYCLE_OFF_SECS);
      else if (key == "ATTENUATION") {
        long configuredAttenuation = val.toInt();
        if (configuredAttenuation < 0) configuredAttenuation = 0;
        if (configuredAttenuation > CONFIG_MAX_ATTENUATION_STEPS) configuredAttenuation = CONFIG_MAX_ATTENUATION_STEPS;
        settings.attenuation = (uint8_t)configuredAttenuation;
      }
      else if (key == "MORSE_WPM") settings.morseWPM = val.toInt();
      else if (key == "MORSE_FARNSWORTH_WPM") settings.farnsworthWPM = val.toInt();
      else if (key == "MORSE_TONE") settings.morseToneHz = val.toInt();
      else if (key == "MORSE_TONE_VOL") settings.toneAmplitudePercent = val.toInt();
      line = "";
    } else {
      line += ch;
    }
  }
  bool readClean = !file.getError();
  bool fileClosed = file.close();
  bool rootClosed = closeRoot();
  if (!readClean || !fileClosed || !rootClosed) return false;

  // Validate config and apply defaults when config is weird.
  if (settings.dutyCyclePercent > 100) {
    settings.dutyCyclePercent = DEFAULT_DUTY_CYCLE;
  }
  if (settings.attenuation > CONFIG_MAX_ATTENUATION_STEPS) {
    settings.attenuation = CONFIG_MAX_ATTENUATION_STEPS;
  }
  if (settings.morseWPM == 0 || settings.morseWPM > 30) {
    settings.morseWPM = DEFAULT_WPM;
  }
  if (settings.farnsworthWPM == 0 || settings.farnsworthWPM < settings.morseWPM) {
    settings.farnsworthWPM = settings.morseWPM;
  }
  if (settings.morseToneHz < 100 || settings.morseToneHz >= 2500) {
    settings.morseToneHz = DEFAULT_MORSE_TONE_HZ;
  }
  if (settings.toneAmplitudePercent == 0 || settings.toneAmplitudePercent > 100) {
    settings.toneAmplitudePercent = DEFAULT_TONE_AMPLITUDE_PERCENT;
  }

  // Disable transmitter if no callsign is set.
  settings.isConfigured = (strlen(settings.callsign) > 0);

  // Disable transmitter if transmit frequency is out of band.
  uint32_t transmitFreqHz = settings.transmitFreqMHz * 1e6;
  uint16_t deviationMarginHz = FM_DEVIATION_HZ * 2;
  uint32_t minFreqHz = MIN_FREQ_MHZ * 1e6 + deviationMarginHz;
  uint32_t maxFreqHz;
  if (settings.ituZone == 1) {
    maxFreqHz = MAX_FREQ_MHZ_ITU1 * 1e6 - deviationMarginHz;
  } else {
    maxFreqHz = MAX_FREQ_MHZ * 1e6 - deviationMarginHz;
  }
  if (transmitFreqHz < minFreqHz || transmitFreqHz > maxFreqHz) {
    settings.isConfigured = false;
  }
  return true;
}

bool readExactFile(const char* filename, void* destination, size_t length) {
  if (!openRoot()) return false;
  if (!file.open(&root, filename, O_RDONLY)) {
    closeRoot();
    return false;
  }
  bool success = file.fileSize() == length &&
                 file.read(destination, length) == (int)length &&
                 !file.getError();
  bool fileClosed = file.close();
  bool rootClosed = closeRoot();
  return success && fileClosed && rootClosed;
}

// Generates an audio morse code ID from the provided callsign.
bool generateMorseAudio() {
  VerifiedFileWrite state;
  if (!beginVerifiedFileWrite(CALLSIGN_WAV, &state)) return false;
  bool writeSucceeded = writeVerifiedChunk(&state, wavHeader, sizeof(wavHeader));

  const int sampleRate = AUDIO_SAMPLE_RATE_HZ;
  const int toneFreq = settings.morseToneHz;
  const double ditLengthSec = 1.2f / settings.morseWPM;
  const double interCharLengthSec = 1.2f / settings.farnsworthWPM * 3;
  const double riseFall = (1.0f / 3.0f) * ditLengthSec * sampleRate;

  const int amplitude = (settings.toneAmplitudePercent * 32767) / 100;
  const double toneStep = 2.0f * PI * toneFreq / sampleRate;

  auto writeTone = [&](double durationSec) {
    int samples = (int)(durationSec * sampleRate);
    for (int i = 0; writeSucceeded && i < samples; i++) {
      double envelope = 1.0f;
      if (i < riseFall) envelope = i / riseFall;
      else if (i > samples - riseFall) envelope = (samples - i) / riseFall;
      int16_t sample = (int16_t)(amplitude * envelope * sinf(toneStep * i));
      writeSucceeded = writeVerifiedChunk(&state, &sample, sizeof(sample));
    }
  };

  auto writeSilence = [&](double durationSec) {
    int samples = (int)(durationSec * sampleRate);
    int16_t zero = 0;
    for (int i = 0; writeSucceeded && i < samples; i++) {
      writeSucceeded = writeVerifiedChunk(&state, &zero, sizeof(zero));
    }
  };

  const char* morseTable[36] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..",
    ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.",
    "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----."
  };

  auto getMorse = [&](char c) -> const char* {
    if (c >= 'A' && c <= 'Z') return morseTable[c - 'A'];
    if (c >= '0' && c <= '9') return morseTable[c - '0' + 26];
    return "";
  };

  writeSilence(interCharLengthSec);
  for (int i = 0; settings.callsign[i] && i < 12; i++) {
    const char* symbol = getMorse(settings.callsign[i]);
    for (int j = 0; symbol[j]; j++) {
      if (j != 0) writeSilence(ditLengthSec);
      if (symbol[j] == '.') writeTone(ditLengthSec);
      else if (symbol[j] == '-') writeTone(3 * ditLengthSec);
    }
    writeSilence(interCharLengthSec);
  }
  if (!writeSucceeded) {
    abandonVerifiedFileWrite();
    return false;
  }
  if (!finishVerifiedFileWrite(CALLSIGN_WAV, state)) return false;
  return writeVerifiedFile(SETTINGS_CRC, &settingsCrc, sizeof(settingsCrc));
}

// Regenerates morse code ID audio if the callsign has changed.
bool generateMorseIfNeeded() {
  bool callsignExists;
  bool markerExists;
  if (!fileExistsChecked(CALLSIGN_WAV, &callsignExists) ||
      !fileExistsChecked(SETTINGS_CRC, &markerExists)) return false;

  if (callsignExists) {
    uint32_t ignoredSamples;
    if (!wavSampleCount(CALLSIGN_WAV, &ignoredSamples)) return false;
  }

  int savedCrc = 0;
  if (markerExists && !readExactFile(SETTINGS_CRC, &savedCrc, sizeof(savedCrc))) {
    return false;
  }

  if (!callsignExists || !markerExists || savedCrc != settingsCrc) {
    return generateMorseAudio();
  }
  return true;
}

bool saveSettingsCrcMarker() {
  return writeVerifiedFile(SETTINGS_CRC, &settingsCrc, sizeof(settingsCrc));
}

bool validateTimedCycleSettings() {
  if (!wavSampleCount(CALLSIGN_WAV, &callsignAudioSamples)) return false;
  uint32_t minimumOnSeconds =
    (callsignAudioSamples + AUDIO_SAMPLE_RATE_HZ - 1) / AUDIO_SAMPLE_RATE_HZ;
  if (settings.cycleOnSeconds >= minimumOnSeconds) return true;

  settings.cycleOnSeconds = minimumOnSeconds;
  if (!writeSettingValue("CYCLE_ON_SECS", settings.cycleOnSeconds)) return false;
  // Refresh the checksum after changing settings.txt. CYCLE_ON_SECS does not alter
  // the generated ID, so advance its marker without regenerating the WAV.
  return loadSettings() && saveSettingsCrcMarker();
}

// Fully validate and load every file used during startup. Missing base files
// are recreated in place and verified; an I/O failure or invalid existing file
// is promoted to a filesystem failure so the caller can reformat and rebuild.
bool loadFlashData() {
  bool settingsExists;
  bool audioExists;
  uint32_t ignoredSize;
  uint32_t ignoredHash;
  uint32_t ignoredSamples;

  if (!fileExistsChecked(SETTINGS_TXT, &settingsExists)) return false;
  if (!settingsExists) {
    if (!saveDefaultSettings()) return false;
  } else if (!readFileDigest(SETTINGS_TXT, &ignoredSize, &ignoredHash)) {
    return false;
  }

  if (!fileExistsChecked(AUDIO_WAV, &audioExists)) return false;
  if (!audioExists) {
    if (!saveDefaultAudio()) return false;
  } else if (!wavSampleCount(AUDIO_WAV, &ignoredSamples)) {
    return false;
  }

  if (!addMissingTimingSettings()) return false;
  if (!loadSettings()) return false;
  if (!generateMorseIfNeeded()) return false;
  return validateTimedCycleSettings();
}

bool createDefaultFlashData() {
  return saveDefaultSettings() && saveDefaultAudio() && loadFlashData();
}

bool recoverFlashFilesystem() {
  fatfs.end();
  return formatFlashFat16() && flashFilesystemSane() && createDefaultFlashData();
}

bool initializeFlashFilesystem() {
  bool mounted = fatfs.begin(&flash, true, 1, 0);
  if (mounted && flashFilesystemSane() && loadFlashData()) return true;
  return recoverFlashFilesystem();
}

void blinkFilesystemFailure() {
  pinMode(AMP_EN, OUTPUT);
  digitalWrite(AMP_EN, HIGH);
  pinMode(TX_LED, OUTPUT);
  while (true) {
    digitalWrite(TX_LED, HIGH);
    delay(1000);
    digitalWrite(TX_LED, LOW);
    delay(1000);
  }
}

uint64_t configuredCarrierCentiHz() {
  return (uint64_t)((settings.transmitFreqMHz * 100000000.0) + 0.5);
}

// Returns the closest fraction to numerator/denominator with a denominator no greater than maxDenominator.
RationalFraction bestRationalApproximation(uint64_t numerator, uint64_t denominator, uint32_t maxDenominator) {
  if (numerator == 0) {
    return {0, 1};
  }

  const uint64_t originalNumerator = numerator;
  const uint64_t originalDenominator = denominator;
  uint64_t previousNumerator = 0;
  uint64_t previousDenominator = 1;
  uint64_t currentNumerator = 1;
  uint64_t currentDenominator = 0;

  while (denominator != 0) {
    uint64_t coefficient = numerator / denominator;

    // Stop before the next convergent exceeds the SI5351 denominator limit.
    if (currentDenominator != 0 && coefficient > (maxDenominator - previousDenominator) / currentDenominator) {
      break;
    }

    uint64_t nextNumerator = previousNumerator + coefficient * currentNumerator;
    uint64_t nextDenominator = previousDenominator + coefficient * currentDenominator;
    previousNumerator = currentNumerator;
    previousDenominator = currentDenominator;
    currentNumerator = nextNumerator;
    currentDenominator = nextDenominator;

    uint64_t remainder = numerator % denominator;
    numerator = denominator;
    denominator = remainder;
  }

  if (denominator == 0) {
    return {(uint32_t)currentNumerator, (uint32_t)currentDenominator};
  }

  uint64_t scale = (maxDenominator - previousDenominator) / currentDenominator;
  uint64_t boundedNumerator = previousNumerator + scale * currentNumerator;
  uint64_t boundedDenominator = previousDenominator + scale * currentDenominator;

  // Only this final comparison uses floating point. The continued-fraction search itself is exact integer arithmetic.
  double target = (double)originalNumerator / (double)originalDenominator;
  double boundedError = fabs(((double)boundedNumerator / (double)boundedDenominator) - target);
  double convergentError = fabs(((double)currentNumerator / (double)currentDenominator) - target);
  if (convergentError <= boundedError) {
    return {(uint32_t)currentNumerator, (uint32_t)currentDenominator};
  }
  return {(uint32_t)boundedNumerator, (uint32_t)boundedDenominator};
}

// Calculates the eight PLLA parameter bytes for one signed 16-bit audio sample.
void calculatePllRegisters(int16_t sample, uint64_t carrierCentiHz, uint8_t* params) {
  const uint64_t audioScale = 32767;
  int64_t deviationCentiHzNumerator = (int64_t)sample * FM_DEVIATION_HZ * 100;
  uint64_t rfCentiHzNumerator = (uint64_t)((int64_t)(carrierCentiHz * audioScale) + deviationCentiHzNumerator);
  uint64_t feedbackNumerator = rfCentiHzNumerator * SI5351_CLOCK_DIV;
  uint64_t feedbackDenominator = SI5351_REFERENCE_HZ * 100ULL * audioScale;

  uint32_t a = feedbackNumerator / feedbackDenominator;
  uint64_t fractionalNumerator = feedbackNumerator % feedbackDenominator;
  RationalFraction fraction = bestRationalApproximation(fractionalNumerator, feedbackDenominator, SI5351_PLL_MAX_DENOM);
  uint32_t b = fraction.numerator;
  uint32_t c = fraction.denominator;

  // Handle a rounded fraction of 1/1, although the configured PicoFox frequency range should never require it.
  if (b >= c) {
    a++;
    b = 0;
    c = 1;
  }

  uint32_t fractional128 = (128U * b) / c;
  uint32_t p1 = 128U * a + fractional128 - 512U;
  uint32_t p2 = 128U * b - c * fractional128;
  uint32_t p3 = c;

  params[0] = (p3 >> 8) & 0xff;
  params[1] = p3 & 0xff;
  params[2] = (p1 >> 16) & 0x03;
  params[3] = (p1 >> 8) & 0xff;
  params[4] = p1 & 0xff;
  params[5] = ((p3 >> 12) & 0xf0) | ((p2 >> 16) & 0x0f);
  params[6] = (p2 >> 8) & 0xff;
  params[7] = p2 & 0xff;
}

uint32_t updateCrc32(uint32_t crc, uint8_t value) {
  crc ^= value;
  for (uint8_t bit = 0; bit < 8; bit++) {
    crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}

void copyFromLutFlash(void* destination, const volatile void* source, size_t length) {
  uint8_t* destinationBytes = (uint8_t*)destination;
  const volatile uint8_t* sourceBytes = (const volatile uint8_t*)source;
  for (size_t i = 0; i < length; i++) {
    destinationBytes[i] = sourceBytes[i];
  }
}

bool lutHeaderMatches(const Si5351LutHeader& header, uint64_t carrierCentiHz) {
  return header.magic == SI5351_LUT_MAGIC &&
         header.version == SI5351_LUT_VERSION &&
         header.entrySize == SI5351_LUT_ENTRY_SIZE &&
         header.entryCount == SI5351_LUT_ENTRY_COUNT &&
         header.carrierCentiHz == carrierCentiHz &&
         header.referenceHz == SI5351_REFERENCE_HZ &&
         header.deviationHz == FM_DEVIATION_HZ &&
         header.clockDiv == SI5351_CLOCK_DIV &&
         header.maxDenominator == SI5351_PLL_MAX_DENOM;
}

bool validateFrequencyLut(uint64_t carrierCentiHz) {
  Si5351LutHeader header;
  copyFromLutFlash(&header, si5351LutStorage.headerSector, sizeof(header));
  if (!lutHeaderMatches(header, carrierCentiHz)) {
    return false;
  }

  uint32_t crc = 0xffffffffU;
  const volatile uint8_t* data = &si5351LutStorage.entries[0][0];
  for (uint32_t i = 0; i < SI5351_LUT_ENTRY_COUNT * SI5351_LUT_ENTRY_SIZE; i++) {
    crc = updateCrc32(crc, data[i]);
  }
  return (crc ^ 0xffffffffU) == header.dataCrc32;
}

uint32_t frequencyLutFlashOffset() {
  return (uint32_t)((uintptr_t)&si5351LutStorage - XIP_BASE);
}

void eraseLutFlashSector(uint32_t flashOffset) {
  noInterrupts();
  flash_range_erase(flashOffset, FLASH_SECTOR_SIZE);
  interrupts();
}

void programLutFlashSector(uint32_t flashOffset, const uint8_t* data) {
  for (uint32_t pageOffset = 0; pageOffset < FLASH_SECTOR_SIZE; pageOffset += FLASH_PAGE_SIZE) {
    noInterrupts();
    flash_range_program(flashOffset + pageOffset, data + pageOffset, FLASH_PAGE_SIZE);
    interrupts();
  }
}

bool generateFrequencyLut(uint64_t carrierCentiHz) {
  extern uint8_t _FS_start;
  uintptr_t lutAddress = (uintptr_t)&si5351LutStorage;
  uintptr_t lutEndAddress = lutAddress + sizeof(si5351LutStorage);
  if (lutAddress < XIP_BASE || lutEndAddress > (uintptr_t)&_FS_start) {
    return false;
  }

  const uint32_t lutFlashOffset = frequencyLutFlashOffset();

  // Invalidate the old table before touching its data.
  eraseLutFlashSector(lutFlashOffset);

  uint32_t crc = 0xffffffffU;
  constexpr uint32_t entriesPerSector = FLASH_SECTOR_SIZE / SI5351_LUT_ENTRY_SIZE;
  for (uint32_t firstEntry = 0; firstEntry < SI5351_LUT_ENTRY_COUNT; firstEntry += entriesPerSector) {
    for (uint32_t entryOffset = 0; entryOffset < entriesPerSector; entryOffset++) {
      uint16_t rawSample = (uint16_t)(firstEntry + entryOffset);
      int16_t sample = (int16_t)rawSample;
      uint8_t* entry = &si5351LutSectorBuffer[entryOffset * SI5351_LUT_ENTRY_SIZE];
      calculatePllRegisters(sample, carrierCentiHz, entry);
      for (uint8_t i = 0; i < SI5351_LUT_ENTRY_SIZE; i++) {
        crc = updateCrc32(crc, entry[i]);
      }
    }

    uint32_t dataOffset = FLASH_SECTOR_SIZE + firstEntry * SI5351_LUT_ENTRY_SIZE;
    eraseLutFlashSector(lutFlashOffset + dataOffset);
    programLutFlashSector(lutFlashOffset + dataOffset, si5351LutSectorBuffer);
  }

  Si5351LutHeader header = {
    .magic = SI5351_LUT_MAGIC,
    .version = SI5351_LUT_VERSION,
    .entrySize = SI5351_LUT_ENTRY_SIZE,
    .entryCount = SI5351_LUT_ENTRY_COUNT,
    .carrierCentiHz = carrierCentiHz,
    .referenceHz = SI5351_REFERENCE_HZ,
    .deviationHz = FM_DEVIATION_HZ,
    .clockDiv = SI5351_CLOCK_DIV,
    .maxDenominator = SI5351_PLL_MAX_DENOM,
    .dataCrc32 = crc ^ 0xffffffffU
  };

  // The header is programmed last so an interrupted generation never validates as complete.
  memset(si5351LutSectorBuffer, 0xff, sizeof(si5351LutSectorBuffer));
  memcpy(si5351LutSectorBuffer, &header, sizeof(header));
  programLutFlashSector(lutFlashOffset, si5351LutSectorBuffer);
  return validateFrequencyLut(carrierCentiHz);
}

bool ensureFrequencyLut() {
  uint64_t carrierCentiHz = configuredCarrierCentiHz();
  if (validateFrequencyLut(carrierCentiHz)) {
    return true;
  }
  return generateFrequencyLut(carrierCentiHz);
}

// Starts the shared I2C bus if it is not already running.
void startWire() {
  if (wireRunning) return;

  // Do not rely on the Arduino board variant's default Wire pins. PicoFox uses
  // GPIO0/GPIO1, while the generic RP2040 variant defaults to GPIO4/GPIO5.
  Wire.setSDA(SDA);
  Wire.setSCL(SCL);
  Wire.begin();
  Wire.setClock(I2C_CLOCK_HZ);
  wireRunning = true;
}

// Stops the shared I2C bus so the attenuator can reclaim the pins.
void stopWire() {
  if (!wireRunning) return;

  Wire.end();
  wireRunning = false;
}

constexpr uint8_t hardwareAttenuationForConfig(uint8_t configuredAttenuation) {
  return configuredAttenuation >= SI5351_LOW_DRIVE_ATTENUATION_STEPS
           ? configuredAttenuation - SI5351_LOW_DRIVE_ATTENUATION_STEPS
           : configuredAttenuation;
}

static_assert(hardwareAttenuationForConfig(23) == 23, "4mA attenuation mapping is incorrect");
static_assert(hardwareAttenuationForConfig(24) == 0, "2mA attenuation transition is incorrect");
static_assert(hardwareAttenuationForConfig(151) == 127, "Maximum attenuation mapping is incorrect");

// Programs the BVA1761 attenuator. This temporarily reclaims the shared SDA/SCL pins.
void programAttenuator(uint8_t attenuation) {
  if (revision != 3) return;

  if (attenuation > ATTENUATOR_MAX_STEPS) {
    attenuation = ATTENUATOR_MAX_STEPS;
  }

  stopWire();

  pinMode(ATTN_LE, OUTPUT);
  digitalWrite(ATTN_LE, LOW);

  // Hold SCL low before touching SDA so the shared bus does not see an I2C START.
  pinMode(SCL, OUTPUT);
  digitalWrite(SCL, LOW);
  pinMode(SDA, OUTPUT);
  digitalWrite(SDA, LOW);

  // Attenuation level is first byte LSB first.
  // All address bits zero, written after attn byte.
  uint8_t mask = 1;
  for (int i = 0; i < 16; i++) {
    if (i < 8) {
      if ((attenuation & mask) > 0) {
        pinMode(SDA, INPUT);
      } else {
        pinMode(SDA, OUTPUT);
        digitalWrite(SDA, LOW);
      }
      mask = mask << 1;
    } else {
      pinMode(SDA, OUTPUT);
      digitalWrite(SDA, LOW);
    }

    // Pulse clock to write the current bit.
    pinMode(SCL, INPUT);
    delayMicroseconds(1);
    pinMode(SCL, OUTPUT);
    digitalWrite(SCL, LOW);
    delayMicroseconds(1);
  }

  // Pulse the LE pin to apply the specified attenuation level.
  digitalWrite(ATTN_LE, HIGH);
  delayMicroseconds(1);
  digitalWrite(ATTN_LE, LOW);

  startWire();
}

// Enables and disables the si5351 clock output and the RF amplifier.
void setSi5351Output(bool enabled) {
  if (enabled) {
    si5351.output_enable(SI5351_CLOCK_OUTPUT, 1);
    digitalWrite(AMP_EN, LOW);
    digitalWrite(TX_LED, HIGH);
  } else {
    si5351.output_enable(SI5351_CLOCK_OUTPUT, 0);
    digitalWrite(AMP_EN, HIGH);
    digitalWrite(TX_LED, LOW);
  }
}

// Writes one complete set of precomputed PLLA parameters.
void writePllParameters(uint8_t* params) {
  // Use the library's raw bulk writer so address and transaction behavior are
  // identical to its known-good initialization path.
  si5351.si5351_write_bulk(SI5351_PLL_PARAM_START, SI5351_LUT_ENTRY_SIZE, params);
}

// Applies the precomputed PLLA parameters for a signed 16-bit audio sample.
void setFrequencyForSample(int16_t sample) {
  uint8_t params[SI5351_LUT_ENTRY_SIZE];
  uint16_t tableIndex = (uint16_t)sample;
  copyFromLutFlash(params, si5351LutStorage.entries[tableIndex], sizeof(params));
  writePllParameters(params);
}

// Sleeps until it is time to write the next sample, based on the start of the cycle writing the current sample.
void delayForNextSample(uint32_t start) {
  uint32_t sampleDelta = 1000000UL / AUDIO_SAMPLE_RATE_HZ + SAMPLE_WRITE_CORR_US;

  // At the boundary of micros() overflow. Sleep until the appropriate time AFTER the overflow.
  // Otherwise just sleep for the appropriate delta.
  if ((MAX_UINT32 - start) < sampleDelta) {
    while (micros() > start || micros() < (sampleDelta - (MAX_UINT32 - start)))
      ;
  } else {
    while (micros() - start < sampleDelta)
      ;
  }
}

bool enqueuePllSample(int16_t sample) {
  BufferedCommand command = {};
  command.type = BUFFERED_PLL_WRITE;
  uint16_t tableIndex = (uint16_t)sample;
  copyFromLutFlash(command.pllParams, si5351LutStorage.entries[tableIndex], sizeof(command.pllParams));
  return enqueueBufferedCommand(command);
}

bool enqueueOutputCommand(bool enabled) {
  BufferedCommand command = {};
  command.type = enabled ? BUFFERED_OUTPUT_ON : BUFFERED_OUTPUT_OFF;
  return enqueueBufferedCommand(command);
}

bool enqueueDelay(uint32_t delayMs) {
  BufferedCommand command = {};
  command.type = BUFFERED_DELAY_MS;
  command.delayMs = delayMs;
  return enqueueBufferedCommand(command);
}

bool enqueueFinishRuntime() {
  BufferedCommand command = {};
  command.type = BUFFERED_FINISH_RUNTIME;
  return enqueueBufferedCommand(command);
}

// Reads the actual WAV samples and resolves each one through the flash LUT on
// core 0. Core 1 receives only the exact eight register bytes it must write.
uint32_t bufferAudio(const char* filename, bool* completed = nullptr) {
  if (completed != nullptr) *completed = false;
  openRoot();
  if (!file.open(&root, filename, O_RDWR | O_CREAT)) {
    Serial.println("Failed to open audio file.");
    closeRoot();
    return 0;
  }

  // Load WAV header and validate
  char header[44];
  if (file.read(header, 44) != 44 ||
      strncmp(header, "RIFF", 4) != 0 ||
      strncmp(header + 8, "WAVE", 4) != 0) {
    file.close();
    closeRoot();
    return 0;
  }

  uint32_t numSamples = 0;
  int16_t sample;
  while (file.read((uint8_t*)&sample, 2) == 2) {
    if (!enqueuePllSample(sample)) {
      file.close();
      closeRoot();
      return numSamples * 1000 / AUDIO_SAMPLE_RATE_HZ;
    }
    numSamples++;
  }

  file.close();
  closeRoot();
  if (completed != nullptr) *completed = true;
  return numSamples * 1000 / AUDIO_SAMPLE_RATE_HZ;
}

int16_t fadeAudioBoundary(int16_t sample, uint32_t sampleIndex, uint32_t segmentSamples) {
  uint32_t fadeSamples = min((uint32_t)AUDIO_FADE_SAMPLES, segmentSamples / 2);
  if (fadeSamples == 0) return segmentSamples == 1 ? 0 : sample;

  uint32_t gain = fadeSamples;
  if (sampleIndex < gain) gain = sampleIndex;
  uint32_t samplesAfter = segmentSamples - sampleIndex - 1;
  if (samplesAfter < gain) gain = samplesAfter;
  return (int16_t)(((int32_t)sample * (int32_t)gain) / (int32_t)fadeSamples);
}

bool bufferSilence(uint32_t sampleCount) {
  for (uint32_t i = 0; i < sampleCount; i++) {
    if (!enqueuePllSample(0)) return false;
  }
  return true;
}

// Streams exactly targetSamples from audio.wav without modifying it in flash.
// Each source loop and the final trim boundary receives a short fade to zero.
bool bufferFittedAudio(const char* filename, uint32_t targetSamples) {
  if (targetSamples == 0) return true;

  if (!openRoot()) {
    return bufferSilence(targetSamples);
  }
  if (!file.open(&root, filename, O_RDONLY)) {
    closeRoot();
    return bufferSilence(targetSamples);
  }

  char header[44];
  uint32_t fileSize = file.fileSize();
  bool valid = file.read(header, sizeof(header)) == (int)sizeof(header) &&
               strncmp(header, "RIFF", 4) == 0 &&
               strncmp(header + 8, "WAVE", 4) == 0;
  uint32_t sourceSamples = valid && fileSize >= sizeof(header)
                             ? (fileSize - sizeof(header)) / sizeof(int16_t)
                             : 0;
  if (sourceSamples == 0) {
    file.close();
    closeRoot();
    return bufferSilence(targetSamples);
  }

  uint32_t remainingSamples = targetSamples;
  while (remainingSamples > 0) {
    uint32_t segmentSamples = min(sourceSamples, remainingSamples);
    if (!file.seekSet(sizeof(header))) {
      file.close();
      closeRoot();
      return bufferSilence(remainingSamples);
    }

    for (uint32_t sampleIndex = 0; sampleIndex < segmentSamples; sampleIndex++) {
      int16_t sample = 0;
      file.read((uint8_t*)&sample, sizeof(sample));
      if (!enqueuePllSample(fadeAudioBoundary(sample, sampleIndex, segmentSamples))) {
        file.close();
        closeRoot();
        return false;
      }
    }
    remainingSamples -= segmentSamples;
  }

  file.close();
  closeRoot();
  return true;
}

bool runtimeExpired() {
  return runtimeDeadlineUs != 0 && time_us_64() >= runtimeDeadlineUs;
}

// Leaves RF circuitry off and parks this core in deep sleep. Core 0 will
// either observe runtimeComplete or block on the full command queue and sleep.
void finishRuntime() {
  setSi5351Output(false);
  si5351.set_clock_pwr(SI5351_CLOCK_OUTPUT, 0);
  stopWire();
  runtimeComplete = true;
  __sev();
  sleepForever();
}

void beginRuntimeEnding() {
  setSi5351Output(false);
  runtimeEnding = true;
  __sev();

  // Wait until core 0 has abandoned normal playback. Once it has, discard any
  // cycle commands which were buffered ahead of the deadline and accept only
  // the mandatory final callsign sequence.
  while (!normalProducerStopped) __wfe();
  pllWriteBufferTail = pllWriteBufferHead;
  __dmb();
  finalQueueReady = true;
  __sev();
}

// Timing-critical consumer. It reads only SRAM while producing each sample;
// WAV and LUT flash reads happen concurrently on core 0.
void audioTask() {
  enableProcessorDeepSleep();

  while (!pllWriteStreamStarted || pllWriteBufferDepth() < PLL_WRITE_BUFFER_CAPACITY / 2) {
    __wfe();
  }

  while (true) {
    if (!runtimeEnding && runtimeExpired()) beginRuntimeEnding();

    BufferedCommand command;
    if (!tryDequeueBufferedCommand(command)) {
      __wfe();
      continue;
    }

    if (command.type == BUFFERED_PLL_WRITE) {
      uint32_t start = micros();
      writePllParameters(command.pllParams);
      delayForNextSample(start);
    } else if (command.type == BUFFERED_OUTPUT_ON) {
      setSi5351Output(true);
    } else if (command.type == BUFFERED_OUTPUT_OFF) {
      setSi5351Output(false);
    } else if (command.type == BUFFERED_DELAY_MS) {
      if (runtimeDeadlineUs != 0) {
        uint64_t nowUs = time_us_64();
        uint64_t remainingUs = runtimeDeadlineUs > nowUs ? runtimeDeadlineUs - nowUs : 0;
        uint64_t requestedUs = (uint64_t)command.delayMs * 1000ULL;
        if (remainingUs <= requestedUs) {
          sleep_us(remainingUs);
          beginRuntimeEnding();
          continue;
        }
      }
      delay(command.delayMs);
    } else if (command.type == BUFFERED_FINISH_RUNTIME) {
      finishRuntime();
    }
  }
}

void queueFinalCallsignAndSleep() {
  __dmb();
  normalProducerStopped = true;
  __sev();
  while (!finalQueueReady) __wfe();

  queuingFinalId = true;
  bool callsignComplete = false;
  while (!callsignComplete) {
    enqueuePllSample(0);
    enqueueOutputCommand(true);
    bufferAudio(CALLSIGN_WAV, &callsignComplete);
    enqueueOutputCommand(false);
  }
  enqueueFinishRuntime();
  sleepForever();
}

void setup() {
  // Initialize flash device.
  while (!flash.begin()) {
    Serial.println("Flash setup failed.");
    delay(1000);
  }

  // Do not expose USB storage until the FAT16 volume, every required file, and
  // every startup write have been read back successfully. One failed attempt
  // reformats and rebuilds the complete filesystem; a failed rebuild is fatal.
  bool filesystemReady = initializeFlashFilesystem();
  if (filesystemReady && !checkBootselTrigger()) {
    filesystemReady = recoverFlashFilesystem();
  }
  if (!filesystemReady) blinkFilesystemFailure();

  // Check for test mode before generating the carrier-specific frequency table.
  pinMode(TEST_MODE_PIN, INPUT_PULLDOWN);
  test_mode = digitalRead(TEST_MODE_PIN);
  if (settings.isConfigured || test_mode) {
    frequencyLutReady = ensureFrequencyLut();
  }

  // Init USB mass storage device.
  usb_msc.setID("AI6YM", "PicoFox", "2.1");
  usb_msc.setReadWriteCallback(mscReadCb, mscWriteCb, mscFlushCb);
  usb_msc.setCapacity(flash.size() / 512, 512);
  usb_msc.setUnitReady(true);
  usb_msc.begin();

  // Forces reenumeration when the device is reset.
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  // Setup serial, after USB setup or logs in setup will get eaten.
  Serial.begin(115200);
#ifdef DEBUG
  while (!Serial) delay(10);
  Serial.println("Serial setup complete.");
#endif

  // Initialize board ID pins and ID the board revisions.
  pinMode(ID0, INPUT_PULLDOWN);
  pinMode(ID1, INPUT_PULLDOWN);
  pinMode(ID2, INPUT_PULLDOWN);
  pinMode(ID3, INPUT_PULLDOWN);
  bool id0 = digitalRead(ID0);
  bool id1 = digitalRead(ID1);
  bool id2 = digitalRead(ID2);
  bool id3 = digitalRead(ID3);
  if (!id0 && !id1 && !id2 && !id3) {
    revision = 2;
  } else if (id0 && !id1 && !id2 && !id3) {
    revision = 3;
  } else {
    revision = 0;  // This should never happen, lol.
  }
  Serial.print("Revision: ");
  Serial.println(revision);
  Serial.print("SI5351 frequency LUT: ");
  Serial.println(frequencyLutReady ? "ready" : "unavailable");

  // Initialize pins for LED and Amp control.
  pinMode(AMP_EN, OUTPUT);
  digitalWrite(AMP_EN, HIGH);
  pinMode(TX_LED, OUTPUT);
  digitalWrite(TX_LED, LOW);

  setPlaybackClockSpeed();
  configureRp2040PowerSaving();
  enableProcessorDeepSleep();

  // Test mode must remain immediately available during manufacturing.
  if (!test_mode) sleepForSeconds(settings.initialSleepSeconds);
  if (!test_mode && settings.runtimeSeconds > 0) {
    runtimeDeadlineUs = time_us_64() + (uint64_t)settings.runtimeSeconds * 1000000ULL;
  }

  // Initialize the BVA1761 if present.
  if (revision == 3) {
    programAttenuator(hardwareAttenuationForConfig(settings.attenuation));
  }

  // Initialize the si5351.
  startWire();
  si5351.init(SI5351_CRYSTAL_LOAD_10PF, 0, 0);  // Set load expected by external TCXO.
  si5351.set_clock_pwr(SI5351_CLK1, 0);  // Power down unused output drivers.
  si5351.set_clock_pwr(SI5351_CLK2, 0);
  // Set drive strength.
  if (revision == 3) {
    si5351_drive driveLevel = settings.attenuation >= SI5351_LOW_DRIVE_ATTENUATION_STEPS
                                ? SI5351_DRIVE_2MA
                                : SI5351_DRIVE_LEVEL_R3;
    si5351.drive_strength(SI5351_CLOCK_OUTPUT, driveLevel);
  } else {
    si5351.drive_strength(SI5351_CLOCK_OUTPUT, SI5351_DRIVE_LEVEL_R2);
  }
  if (frequencyLutReady && (settings.isConfigured || test_mode)) {
    uint64_t carrierCentiHz = configuredCarrierCentiHz();
    si5351.set_int(SI5351_CLOCK_OUTPUT, 1);  // Clock will be set to a fixed int mult, fine adjustment done on the PLL mult.
    si5351.set_pll(carrierCentiHz * SI5351_CLOCK_DIV, SI5351_PLL);  // Set the PLL frequency based on the target frequency.
    si5351.set_freq_manual(carrierCentiHz, si5351.plla_freq, SI5351_CLOCK_OUTPUT);  // Let the library calculate and set the simple div by 6 MS.
    si5351.pll_reset(SI5351_PLL);  // Soft reset to get the PLL moving.
    setFrequencyForSample(0);
    setSi5351Output(false);
    Serial.println("si5351 setup complete.");
  }

  // Start the audio playback task on the second core to prevent timing issues.
  multicore_launch_core1(audioTask);
}

void loop() {
  if (runtimeComplete) sleepForever();
  if (runtimeEnding) queueFinalCallsignAndSleep();

  if (frequencyLutReady && settings.isConfigured) {
    enqueuePllSample(0);
    enqueueOutputCommand(true);

    if (settings.dutyCyclePercent == 0) {
      uint32_t idSamples = test_mode ? 0 : callsignAudioSamples;
      uint32_t onSamples = settings.cycleOnSeconds * AUDIO_SAMPLE_RATE_HZ;
      uint32_t audioSamples = onSamples - idSamples;
      bufferFittedAudio(AUDIO_WAV, audioSamples);
      if (!test_mode) bufferAudio(CALLSIGN_WAV);

      if (settings.cycleOffSeconds > 0) {
        enqueueOutputCommand(false);
        enqueueDelay(settings.cycleOffSeconds * 1000UL);
      }
    } else {
      uint32_t audioLengthMs = bufferAudio(AUDIO_WAV);
      if (!test_mode) {  // No generated morse in test mode.
        audioLengthMs += bufferAudio(CALLSIGN_WAV);
      }
      if (settings.dutyCyclePercent < 100) {
        uint32_t offTime = (uint32_t)(((uint64_t)(100 - settings.dutyCyclePercent) * audioLengthMs) /
                                      settings.dutyCyclePercent);
        enqueueOutputCommand(false);
        enqueueDelay(offTime);
      }
    }
  } else {
    delay(1000);
  }
}
