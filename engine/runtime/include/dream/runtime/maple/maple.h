// Maple bus: the DMA engine at 0x005F6C00 that walks the game's transfer
// descriptors, hands each frame to the device on that port, and writes the reply frames back
// into RAM on the virtual clock, plus a standard controller. Protocol and device responses follow
// Flycast's hw/maple (GPL-2.0) and the Katana Maple specification's frame layout.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dream/runtime/holly/intc.h"
#include "dream/runtime/mem/dc_memory.h"
#include "dream/runtime/sched/scheduler.h"

namespace dream::maple {

enum Command : std::uint8_t {
    kDeviceRequest = 0x01,
    kAllStatusReq = 0x02,
    kDeviceReset = 0x03,
    kDeviceKill = 0x04,
    kGetCondition = 0x09,
    kGetMediaInfo = 0x0A,
    kBlockRead = 0x0B,
    kBlockWrite = 0x0C,
    kGetLastError = 0x0D,
    kSetCondition = 0x0E,
};
enum Reply : std::uint8_t {
    kDeviceStatus = 0x05,
    kDeviceStatusAll = 0x06,
    kDeviceReply = 0x07,
    kDataTransfer = 0x08,
    kFileError = 0xFB,
    kTransmitAgain = 0xFC,
    kUnknownCommand = 0xFD,
    kUnknownFunction = 0xFE,
};
enum Function : std::uint32_t {
    kInput = 0x01000000,
    kStorage = 0x02000000,
    kLcd = 0x04000000,
    kClock = 0x08000000,
    kVibration = 0x00010000,
};

// Payload builder for reply frames (words after the header, little-endian bytes).
class Payload {
public:
    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void str(const char* s, std::size_t field);  // space-padded fixed-width string
    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

class Device {
public:
    virtual ~Device() = default;
    virtual std::uint32_t functions() const noexcept = 0;
    // Handles one frame: `args` are the words after the frame header. Returns the reply code and
    // fills `out` with the reply payload (a multiple of 4 bytes).
    virtual Reply handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                         Payload& out) = 0;
};

// A host input's travel, 0 to 1 per direction, as the controller's own byte. The pair is one axis:
// `low` and `high` are the two halves a player binds separately, so a keyboard binding two keys and
// a pad binding two halves of one stick arrive here the same way. 0x80 is centre, and two digital
// sources still give the 0x00 and 0xFF extremes a keyboard has always produced.
std::uint8_t axis_byte(float low, float high) noexcept;
// A trigger's travel as its byte: 0 released, 0xFF fully down, and everything between preserved,
// which is the difference a real pad makes to a driving game.
std::uint8_t trigger_byte(float v) noexcept;

// The standard controller. Buttons are active-low (1 = released) in Katana's order:
// C B A Start Up Down Left Right Z Y X D Up2 Down2 Left2 Right2 (bits 0..15).
struct ControllerState {
    std::uint16_t buttons = 0xFFFF;
    std::uint8_t rtrigger = 0, ltrigger = 0;
    std::uint8_t joy_x = 0x80, joy_y = 0x80, joy2_x = 0x80, joy2_y = 0x80;
};
enum Button : std::uint16_t {
    kC = 1 << 0,
    kB = 1 << 1,
    kA = 1 << 2,
    kStart = 1 << 3,
    kUp = 1 << 4,
    kDown = 1 << 5,
    kLeft = 1 << 6,
    kRight = 1 << 7,
    kZ = 1 << 8,
    kY = 1 << 9,
    kX = 1 << 10,
    kD = 1 << 11,
};

class Controller final : public Device {
public:
    ControllerState state;
    std::uint32_t functions() const noexcept override { return kInput; }
    Reply handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                 Payload& out) override;
};

// The vibration pack ("Puru Puru"), in a controller's expansion slot. The game
// sets a vibration condition; the pack reports it to the host through `on_vibrate` as a strength
// 0..1 and a duration, which a window maps onto its gamepad's rumble. Protocol as documented by
// Flycast's implementation: the condition word carries CNT (bit 0), positive/negative power (bits
// 8-10 / 12-14), EXH/INH (bits 11 / 15), frequency (bits 16-23) and inclination (bits 24-31); a
// block write sets the auto-stop time in 250 ms steps.
class VibrationPack final : public Device {
public:
    std::function<void(float strength, unsigned duration_ms)> on_vibrate;
    std::uint32_t functions() const noexcept override { return kVibration; }
    Reply handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                 Payload& out) override;
    std::uint32_t condition = 0;
    unsigned auto_stop_ms = 5000;
    std::uint64_t vibrations = 0;
};

// A visual memory unit, as far as a game is concerned: 128 KB of flash in 256 blocks of 512
// bytes, read and written a block at a time, with a small screen and a clock the game may also
// address. Games use it to save, and some refuse to record anything without one.
//
// The image is the same 128 KB layout every Dreamcast tool uses, so a save made in an emulator can
// be used directly. Writes go back to the file so progress survives a run. Never commit one: it
// holds the player's saves.
// What load() found. A missing file is not an error: the caller decides whether to make one, and
// only the caller knows whether the user asked for that.
enum class CardStatus { Ok, Missing, WrongSize, Unreadable };

class MemoryCard final : public Device {
public:
    static constexpr std::size_t kBlocks = 256, kBlockSize = 512;
    static constexpr std::size_t kImageSize = kBlocks * kBlockSize;

    // Loads an image, and remembers the path to save back to only when it really is a card.
    //
    // The size check is the point. save() rewrites the whole 128 KB after every block write, so a
    // path that is not a card would be truncated the moment the title saved. Anything that is not
    // exactly kImageSize bytes is refused, and no path is remembered, so nothing can be written
    // back over it.
    CardStatus load(const std::string& path);
    // Writes the image back; called automatically after each block write when a path is set.
    // False when there is no path, which is what a refused load leaves behind.
    bool save() const;
    // Writes to `path` and adopts it. For creating a card that did not exist yet.
    bool save_as(const std::string& path);

    // Makes this a blank, formatted card: the system block's geometry, a FAT with the whole user
    // area free and the directory chain allocated, and an empty directory, laid out as on a real
    // card.
    void format();

    std::uint32_t functions() const noexcept override { return kStorage | kLcd | kClock; }
    Reply handle(std::uint8_t command, const std::uint32_t* args, std::size_t nwords,
                 Payload& out) override;

    bool formatted() const noexcept;
    const std::vector<std::uint8_t>& image() const noexcept { return flash_; }
    // Block access without the Maple protocol, for native code that implements the game's backup
    // library. write_block saves the file, as a Maple block write does. False for a block past the
    // end, or when the file cannot be written.
    bool read_block(unsigned block, std::uint8_t* out) const;
    bool write_block(unsigned block, const std::uint8_t* data);

private:
    std::vector<std::uint8_t> flash_ = std::vector<std::uint8_t>(kImageSize, 0);
    std::string path_;
};

class Bus final : public mem::MmioHandler {
public:
    static constexpr std::uint32_t kBase = 0x005F6C00u, kEnd = 0x005F6D00u;
    static constexpr unsigned kPorts = 4;

    Bus(sched::Scheduler& sched, holly::Intc& holly, mem::DcMemory& memory);
    void reset();
    void attach(unsigned port, std::unique_ptr<Device> device);
    // Each controller port carries up to five expansion slots, addressed by one bit each, where
    // memory cards and rumble packs live. Slot 0 is where a memory card normally sits.
    static constexpr unsigned kExpansionSlots = 5;
    void attach_expansion(unsigned port, unsigned slot, std::unique_ptr<Device> device);
    // The device a frame's recipient byte names: bit 5 is the main device, bits 0 to 4 the slots.
    Device* device_for(unsigned port, std::uint8_t recipient) const noexcept;
    Device* device(unsigned port) const noexcept { return ports_[port].get(); }

    // Called by the SPG at VBlank-out: starts the transfer when the hardware trigger is selected.
    void vblank();

    std::uint32_t read(std::uint32_t addr, unsigned size) override;
    void write(std::uint32_t addr, std::uint32_t value, unsigned size) override;

    // Registers
    std::uint32_t mdstar = 0, mdtsel = 0, mden = 0, mdst = 0, msys = 0x3A980000, mshtcl = 0,
                  mdapro = 0x00007F00, mmsel = 1;

private:
    void start_dma();
    void complete();
    struct Pending {
        std::uint32_t addr;
        std::vector<std::uint32_t> words;
    };
    sched::Scheduler& sched_;
    holly::Intc& holly_;
    mem::DcMemory& memory_;
    std::array<std::unique_ptr<Device>, kPorts> ports_;
    std::array<std::array<std::unique_ptr<Device>, kExpansionSlots>, kPorts> expansion_;

public:

private:
    std::vector<Pending> out_;
    int event_ = -1;
    bool trigger_pending_reset_ = false;
};

}  // namespace dream::maple
