// rksys.dat license editing (layout: mkwiiki.org/wiki/Rksys.dat).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <zlib.h>

#include "data/game_paths.h"
#include "data/text_utf16.h"
#include "mkw/data.h"

namespace fs = std::filesystem;

namespace mkw {
namespace {

constexpr std::size_t kFileSize = 0x2BC000;
constexpr std::size_t kLicenseBase = 0x08;
constexpr std::size_t kLicenseSize = 0x8CC0;
constexpr int kLicenses = 4;
constexpr std::size_t kCrcOffset = 0x27FFC;  // CRC-32 over everything before it

// Inside an RKPD license block.
constexpr std::size_t kName = 0x14;          // 10 UTF-16BE characters
constexpr std::size_t kAvatarId = 0x28;
constexpr std::size_t kClientId = 0x2C;
constexpr std::size_t kUnlockFirst = 0x30;   // cups, characters, engine classes, vehicles
constexpr std::size_t kUnlockLast = 0x38;
constexpr std::size_t kWins = 0x88;
constexpr std::size_t kLosses = 0x8C;
constexpr std::size_t kVr = 0xB0;
constexpr std::size_t kBr = 0xB2;

enum class CrcOrder { Unknown, Big, Little };

std::uint32_t ComputeCrc(const std::vector<std::uint8_t>& f) {
    return static_cast<std::uint32_t>(crc32(0L, f.data(), static_cast<uInt>(kCrcOffset)));
}

class LicenseServiceImpl : public LicenseService {
public:
    bool Load(Variant v, std::string* error) override {
        file_.clear();
        dirty_ = false;
        variant_ = v;
        loaded_ = false;
        std::ifstream in(paths::SavePath(v), std::ios::binary);
        if (!in) {
            if (error) *error = "No save yet for this game. Play once to create one.";
            return false;
        }
        file_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (file_.size() != kFileSize || std::string(file_.begin(), file_.begin() + 4) != "RKSD") {
            if (error) *error = "The save file is not a Mario Kart Wii rksys.dat.";
            file_.clear();
            return false;
        }
        const std::uint32_t crc = ComputeCrc(file_);
        const std::uint8_t* c = file_.data() + kCrcOffset;
        const std::uint32_t be = text::Read32(c);
        const std::uint32_t le = static_cast<std::uint32_t>(c[0]) | (c[1] << 8) | (c[2] << 16) | (static_cast<std::uint32_t>(c[3]) << 24);
        order_ = crc == be ? CrcOrder::Big : crc == le ? CrcOrder::Little : CrcOrder::Unknown;
        loaded_ = true;
        return true;
    }

    bool Loaded() const override { return loaded_; }
    Variant LoadedVariant() const override { return variant_; }
    bool Writable() const override { return loaded_ && order_ != CrcOrder::Unknown; }

    std::vector<LicenseEntry> List() const override {
        std::vector<LicenseEntry> out;
        if (!loaded_) return out;
        for (int i = 0; i < kLicenses; ++i) {
            const std::uint8_t* l = Block(i);
            LicenseEntry e;
            e.index = i;
            e.name = text::ReadName(l + kName, 10);
            e.used = std::string(l, l + 4) == "RKPD" && !e.name.empty();
            e.vr = text::Read16(l + kVr);
            e.br = text::Read16(l + kBr);
            e.wins = text::Read32(l + kWins);
            e.losses = text::Read32(l + kLosses);
            e.avatarId = text::Read32(l + kAvatarId);
            e.clientId = text::Read32(l + kClientId);
            out.push_back(e);
        }
        return out;
    }

    bool SetName(int index, const std::string& name) override {
        if (!Editable(index)) return false;
        if (!text::WriteName(Block(index) + kName, 10, name)) return false;
        dirty_ = true;
        return true;
    }

    bool SetRatings(int index, int vr, int br) override {
        if (!Editable(index)) return false;
        text::Write16(Block(index) + kVr, static_cast<std::uint16_t>(std::clamp(vr, 1, 9999)));
        text::Write16(Block(index) + kBr, static_cast<std::uint16_t>(std::clamp(br, 1, 9999)));
        dirty_ = true;
        return true;
    }

    bool UnlockEverything(int index) override {
        if (!Editable(index)) return false;
        std::uint8_t* l = Block(index);
        for (std::size_t o = kUnlockFirst; o <= kUnlockLast; ++o) l[o] = 0xFF;
        dirty_ = true;
        return true;
    }

    bool LinkMii(int index, const MiiEntry& mii) override {
        if (!Editable(index)) return false;
        std::uint8_t* l = Block(index);
        if (!text::WriteName(l + kName, 10, mii.name)) return false;
        text::Write32(l + kAvatarId, mii.miiId);
        text::Write32(l + kClientId, mii.systemId);
        dirty_ = true;
        return true;
    }

    bool Dirty() const override { return dirty_; }

    bool Save(std::string* error) override {
        if (!Writable()) {
            if (error) *error = "This save's checksum format is not recognised; it was not changed.";
            return false;
        }
        const std::uint32_t crc = ComputeCrc(file_);
        std::uint8_t* c = file_.data() + kCrcOffset;
        if (order_ == CrcOrder::Big) {
            text::Write32(c, crc);
        } else {
            for (int b = 0; b < 4; ++b) c[b] = static_cast<std::uint8_t>(crc >> (8 * b));
        }

        if (!Saves().BackupCurrent(variant_)) {
            if (error) *error = "Could not back up the save first; nothing was changed.";
            return false;
        }
        std::error_code ec;
        const std::string live = paths::SavePath(variant_);
        const std::string part = live + ".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(file_.data()), static_cast<std::streamsize>(file_.size()));
            out.close();
            if (!out) {
                fs::remove(part, ec);
                if (error) *error = "Could not write the save.";
                return false;
            }
        }
        const std::string bak = live + ".bak";
        fs::remove(bak, ec);
        fs::rename(live, bak, ec);
        if (ec) {
            fs::remove(part, ec);
            if (error) *error = "Could not replace the save.";
            return false;
        }
        fs::rename(part, live, ec);
        if (ec) {
            std::error_code rollback;
            fs::rename(bak, live, rollback);
            if (error) *error = "Could not replace the save; the original was put back.";
            return false;
        }
        fs::remove(bak, ec);
        dirty_ = false;
        return true;
    }

private:
    std::uint8_t* Block(int i) { return file_.data() + kLicenseBase + static_cast<std::size_t>(i) * kLicenseSize; }
    const std::uint8_t* Block(int i) const { return file_.data() + kLicenseBase + static_cast<std::size_t>(i) * kLicenseSize; }

    bool Editable(int index) const {
        return Writable() && index >= 0 && index < kLicenses && std::string(Block(index), Block(index) + 4) == "RKPD";
    }

    std::vector<std::uint8_t> file_;
    Variant variant_ = Variant::Vanilla;
    CrcOrder order_ = CrcOrder::Unknown;
    bool loaded_ = false;
    bool dirty_ = false;
};

}  // namespace

LicenseService& Licenses() {
    static LicenseServiceImpl instance;
    return instance;
}

}  // namespace mkw
