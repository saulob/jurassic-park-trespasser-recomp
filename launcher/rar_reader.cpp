#include "rar_reader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>

#include "archive_spool.h"
#include "os_file.h"
#include "rar_blake2.h"
#include "rar_stream.h"
#include "rar_unpack.h"
#include "third_party/lzma/7zCrc.h"

namespace fs = std::filesystem;

namespace jpt {
namespace {

constexpr uint64_t kDictionary4 = 4ull << 20;
constexpr uint64_t kMostDictionary = 256ull << 20;
constexpr uint64_t kMostHeader = 2ull << 20;
constexpr size_t kMostName = 4096;
constexpr uint32_t kUnixType = 0170000, kUnixLink = 0120000;
constexpr uint32_t kWindowsDirectory = 0x10, kWindowsLink = 0x400;

// The fields of a header, in order; past its end it is not ok.
class Fields {
public:
    Fields(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    uint64_t Little(size_t bytes) {
        uint64_t value = 0;
        if (!Room(bytes)) return 0;
        for (size_t i = 0; i < bytes; ++i) value |= uint64_t(data_[at_++]) << (8 * i);
        return value;
    }
    uint32_t U8() { return uint32_t(Little(1)); }
    uint32_t U16() { return uint32_t(Little(2)); }
    uint32_t U32() { return uint32_t(Little(4)); }
    uint64_t Vint() {
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const uint32_t byte = U8();
            value |= uint64_t(byte & 0x7F) << shift;
            if (!(byte & 0x80)) return value;
        }
        ok_ = false;
        return 0;
    }
    const uint8_t* Bytes(uint64_t count) {
        if (!Room(count)) return data_;
        const uint8_t* from = data_ + at_;
        at_ += size_t(count);
        return from;
    }
    Fields Part(uint64_t count) {
        const uint8_t* from = Bytes(count);
        return Fields(from, ok_ ? size_t(count) : 0);
    }
    // The last bytes, taken away from what follows here.
    Fields Tail(uint64_t count) {
        if (!Room(count)) return Fields(data_, 0);
        size_ -= size_t(count);
        return Fields(data_ + size_, size_t(count));
    }
    size_t at() const { return at_; }
    size_t left() const { return size_ - at_; }
    bool ok() const { return ok_; }

private:
    bool Room(uint64_t count) {
        if (count > size_ - at_) ok_ = false;
        return ok_;
    }
    const uint8_t* data_;
    size_t size_, at_ = 0;
    bool ok_ = true;
};

struct Item {
    uint64_t offset = 0, packed = 0, dictionary = 0;
    bool stored = false, solid = false;
    std::optional<rar::Method> method;
    std::optional<uint32_t> crc;
    std::optional<std::array<uint8_t, 32>> blake;
    std::optional<size_t> copy_of;
    ArchiveError error = ArchiveError::kNone;
};

// The bytes of the file as they come out, against what its header says of them.
class Verifier {
public:
    explicit Verifier(const Item& item) : item_(item) {}
    void Update(const uint8_t* data, size_t size) {
        crc_ = CrcUpdate(crc_, data, size);
        if (item_.blake) blake_.Update(data, size);
    }
    ArchiveError Finish(uint64_t size) {
        if (!item_.crc && !item_.blake && size) return ArchiveError::kUnsupported;
        const bool ok = (!item_.crc || *item_.crc == CRC_GET_DIGEST(crc_)) && (!item_.blake || *item_.blake == blake_.Finish());
        return ok ? ArchiveError::kNone : ArchiveError::kDamaged;
    }

private:
    const Item& item_;
    uint32_t crc_ = CRC_INIT_VAL;
    rar::Blake2sp blake_;
};

// The first file asked for expands them all, in order.
class RarReader final : public ArchiveReader {
public:
    ArchiveError Open(const fs::path& archive, const ArchiveLimits& limits) {
        static const bool crc_ready = (CrcGenerateTable(), true);
        (void)crc_ready;
        limits_ = limits;
        uint8_t magic[8];
        if (!file_.Open(archive) || !file_.Size(size_)) return ArchiveError::kUnreadable;
        if (size_ < sizeof magic) return ArchiveError::kDamaged;
        if (!file_.ReadAt(0, magic, sizeof magic)) return ArchiveError::kUnreadable;
        if (!std::memcmp(magic, "Rar!\x1A\x07\x01\x00", 8)) return List(8, &RarReader::Block5);
        if (!std::memcmp(magic, "Rar!\x1A\x07\x00", 7)) return List(7, &RarReader::Block4);
        return ArchiveError::kUnsupported;
    }

    ArchiveError Extract(size_t index, const Sink& sink) override {
        if (!expanded_) {
            expanded_ = true;
            error_ = Expand();
            file_.Close();
        }
        if (error_ != ArchiveError::kNone) return error_;
        const Item* item = &items_[index];
        if (item->copy_of) item = &items_[index = *item->copy_of];
        if (item->error != ArchiveError::kNone) return item->error;
        return spool_.Read(index, entries_[index].size, sink);
    }

private:
    using Block = ArchiveError (RarReader::*)(uint64_t& at, bool& end);

    ArchiveError List(uint64_t at, Block block) {
        bool end = false;
        while (!end && at < size_) {
            const ArchiveError error = (this->*block)(at, end);
            if (error != ArchiveError::kNone) return error;
        }
        return ArchiveError::kNone;
    }

    ArchiveError Add(ArchiveEntry entry, Item item) {
        if (entry.name.empty() || entry.name.size() > kMostName || entry.name.find('\0') != std::string::npos) return ArchiveError::kDamaged;
        entry.name = ForwardSlashes(std::move(entry.name));
        if (entry.directory) entry.size = 0;
        if (entries_.size() == limits_.max_entries || entry.size > limits_.max_file_bytes || entry.size > limits_.max_total_bytes - total_) return ArchiveError::kTooLarge;
        total_ += entry.size;
        if (!entry.directory && !item.stored && item.method && item.dictionary <= kMostDictionary) dictionary_ = std::max(dictionary_, item.dictionary);
        by_name_.emplace(entry.name, entries_.size());
        entries_.push_back(std::move(entry));
        items_.push_back(std::move(item));
        return ArchiveError::kNone;
    }

    // ---- RAR 2.0 to 4.x

    ArchiveError Block4(uint64_t& at, bool& end) {
        enum : uint32_t { kMain = 0x73, kFile = 0x74, kService = 0x7A, kEnd = 0x7B };
        constexpr size_t kHead = 7, kMainHead = 13;
        uint8_t head[kHead];
        if (size_ - at < kHead) return ArchiveError::kDamaged;
        if (!file_.ReadAt(at, head, kHead)) return ArchiveError::kUnreadable;
        Fields fixed(head, kHead);
        const uint32_t crc = fixed.U16(), type = fixed.U8(), flags = fixed.U16(), size = fixed.U16();
        if (type < 0x72 || type > kEnd || size < kHead || size > size_ - at) return ArchiveError::kDamaged;
        std::vector<uint8_t> bytes(size);
        if (!file_.ReadAt(at, bytes.data(), size)) return ArchiveError::kUnreadable;
        Fields fields(bytes.data(), size);
        fields.Bytes(kHead);
        // A comment in the header itself, as RAR 2 writes it, is not in the checksum.
        const auto checked = [&](size_t until) { return (CrcCalc(&bytes[2], std::min<size_t>(until, size) - 2) & 0xFFFF) == crc; };
        uint64_t data = 0;
        if (type == kMain) {
            if (!checked(size) && !((flags & 0x02) && checked(kMainHead))) return ArchiveError::kDamaged;
            if (flags & 0x80) return ArchiveError::kUnsupported;
        } else if (type == kFile || type == kService) {
            ArchiveEntry entry;
            Item item;
            const ArchiveError error = File4(fields, flags, entry, item);
            if (!checked(size) && !((flags & 0x08) && checked(fields.at()))) return ArchiveError::kDamaged;
            if (error != ArchiveError::kNone) return error;
            data = item.packed;
            item.offset = at + size;
            if (data > size_ - item.offset) return ArchiveError::kDamaged;
            if (type == kFile) {
                if (flags & 0x03) return ArchiveError::kUnsupported;
                const ArchiveError added = Add(std::move(entry), std::move(item));
                if (added != ArchiveError::kNone) return added;
            }
        } else {
            end = type == kEnd;
            if (end && !checked(size)) return ArchiveError::kDamaged;
            if (end && (flags & 0x01)) return ArchiveError::kUnsupported;
            if (flags & 0x8000) data = fields.U32();
            if (!fields.ok() || (!end && data > size_ - at - size)) return ArchiveError::kDamaged;
        }
        at += size + data;
        return ArchiveError::kNone;
    }

    static ArchiveError File4(Fields& fields, uint32_t flags, ArchiveEntry& entry, Item& item) {
        enum : uint32_t { kEncrypted = 0x04, kSolid = 0x10, kDirectory = 0xE0, kLarge = 0x100, kUnicode = 0x200, kSalt = 0x400 };
        enum : uint32_t { kDos = 0, kOs2 = 1, kWindows = 2, kUnix = 3 };
        item.packed = fields.U32();
        entry.size = fields.U32();
        const uint32_t system = fields.U8();
        item.crc = fields.U32();
        fields.U32();
        const uint32_t version = fields.U8(), method = fields.U8(), name_size = fields.U16(), attributes = fields.U32();
        if (flags & kLarge) {
            item.packed |= uint64_t(fields.U32()) << 32;
            entry.size |= uint64_t(fields.U32()) << 32;
        }
        const uint8_t* name = fields.Bytes(name_size);
        if (flags & kSalt) fields.Bytes(8);
        if (!fields.ok()) return ArchiveError::kDamaged;
        entry.name = Name4(std::string(reinterpret_cast<const char*>(name), name_size), (flags & kUnicode) != 0);
        entry.directory = (flags & kDirectory) == kDirectory || (version < 20 && system <= kWindows && (attributes & kWindowsDirectory));
        entry.link = system == kUnix ? (attributes & kUnixType) == kUnixLink : system <= kWindows && (attributes & kWindowsLink);
        entry.encrypted = (flags & kEncrypted) != 0;
        item.stored = method == 0x30;
        item.solid = (flags & kSolid) != 0;
        item.dictionary = kDictionary4;
        if (method > 0x30 && method <= 0x35) {
            if (version == 20 || version == 26) item.method = rar::Method::k20;
            if (version == 29 || version == 36) item.method = rar::Method::k29;
        }
        return ArchiveError::kNone;
    }

    static std::string Name4(const std::string& raw, bool unicode) {
        const size_t zero = raw.find('\0');
        if (!unicode || zero == std::string::npos) return IsUtf8(raw) ? raw : FromWindows1252(raw);
        // The plain name, then the differences of each character to it.
        const std::string plain = raw.substr(0, zero);
        Fields code(reinterpret_cast<const uint8_t*>(raw.data()) + zero + 1, raw.size() - zero - 1);
        const auto plain_at = [&plain](size_t i) { return i < plain.size() ? uint32_t(uint8_t(plain[i])) : 0u; };
        std::vector<uint16_t> name;
        const uint32_t high = code.U8() << 8;
        uint32_t kinds = 0;
        for (unsigned left = 0; code.left() && name.size() < kMostName;) {
            if (!left) {
                kinds = code.U8();
                left = 4;
                if (!code.left()) break;
            }
            const uint32_t kind = kinds >> 6;
            kinds = kinds << 2 & 0xFF;
            --left;
            if (kind == 0) name.push_back(uint16_t(code.U8()));
            else if (kind == 1) name.push_back(uint16_t(code.U8() | high));
            else if (kind == 2) name.push_back(uint16_t(code.U16()));
            else {
                const uint32_t run = code.U8(), shift = run & 0x80 ? code.U8() : 0;
                for (uint32_t i = (run & 0x7F) + 2; i && name.size() < kMostName; --i) {
                    const uint32_t c = plain_at(name.size());
                    name.push_back(uint16_t(run & 0x80 ? ((c + shift) & 0xFF) | high : c));
                }
            }
        }
        return code.ok() ? FromUtf16(name.data(), name.size()) : std::string();
    }

    // ---- RAR 5

    ArchiveError Block5(uint64_t& at, bool& end) {
        enum : uint64_t { kMain = 1, kFile = 2, kService = 3, kEncryption = 4, kEnd = 5 };
        uint8_t head[7];
        const size_t first = size_t(std::min<uint64_t>(sizeof head, size_ - at));
        if (!file_.ReadAt(at, head, first)) return ArchiveError::kUnreadable;
        Fields fixed(head, first);
        const uint32_t crc = fixed.U32();
        const uint64_t body = fixed.Vint();
        const size_t start = fixed.at();
        if (!fixed.ok() || !body || body > kMostHeader || body > size_ - at - start) return ArchiveError::kDamaged;
        std::vector<uint8_t> bytes(start + size_t(body));
        if (!file_.ReadAt(at, bytes.data(), bytes.size())) return ArchiveError::kUnreadable;
        if (CrcCalc(&bytes[4], bytes.size() - 4) != crc) return ArchiveError::kDamaged;
        Fields fields(bytes.data(), bytes.size());
        fields.Bytes(start);
        const uint64_t type = fields.Vint(), flags = fields.Vint(), extra_size = flags & 1 ? fields.Vint() : 0, data = flags & 2 ? fields.Vint() : 0;
        Fields extra = fields.Tail(extra_size);
        const uint64_t offset = at + bytes.size();
        if (!fields.ok() || data > size_ - offset) return ArchiveError::kDamaged;
        if (type == kEncryption) return ArchiveError::kUnsupported;
        if (type == kFile) {
            ArchiveEntry entry;
            Item item;
            item.offset = offset;
            item.packed = data;
            if (flags & 0x18) return ArchiveError::kUnsupported;
            ArchiveError error = File5(fields, extra, entry, item);
            if (error == ArchiveError::kNone) error = Add(std::move(entry), std::move(item));
            if (error != ArchiveError::kNone) return error;
        } else if (type == kEnd) {
            if (fields.Vint() & 1) return ArchiveError::kUnsupported;
            end = true;
        }
        at = offset + data;
        return ArchiveError::kNone;
    }

    ArchiveError File5(Fields& fields, Fields& extra, ArchiveEntry& entry, Item& item) const {
        enum : uint64_t { kDirectory = 1, kTime = 2, kCrc = 4, kUnknownSize = 8 };
        const uint64_t flags = fields.Vint();
        entry.size = fields.Vint();
        const uint64_t attributes = fields.Vint();
        if (flags & kTime) fields.U32();
        if (flags & kCrc) item.crc = fields.U32();
        const uint64_t compression = fields.Vint(), system = fields.Vint(), name_size = fields.Vint();
        const uint8_t* name = fields.Bytes(name_size);
        if (!fields.ok()) return ArchiveError::kDamaged;
        if (flags & kUnknownSize) return ArchiveError::kUnsupported;
        entry.name.assign(reinterpret_cast<const char*>(name), size_t(name_size));
        if (!IsUtf8(entry.name)) return ArchiveError::kDamaged;
        entry.directory = (flags & kDirectory) != 0;
        entry.link = system == 1 ? (attributes & kUnixType) == kUnixLink : system == 0 && (attributes & kWindowsLink);
        const uint64_t version = compression & 63, method = compression >> 7 & 7;
        item.solid = (compression & 64) != 0;
        item.stored = method == 0;
        item.dictionary = (128ull << 10) << (compression >> 10 & (version ? 31 : 15));
        if (version) item.dictionary += item.dictionary / 32 * (compression >> 15 & 31);
        if (method <= 5 && version == 0) item.method = rar::Method::k50;
        if (method <= 5 && version == 1) item.method = compression >> 20 & 1 ? rar::Method::k50 : rar::Method::k70;
        return Extra5(extra, entry, item);
    }

    ArchiveError Extra5(Fields& extra, ArchiveEntry& entry, Item& item) const {
        enum : uint64_t { kEncryption = 1, kHash = 2, kRedirection = 5 };
        while (extra.ok() && extra.left()) {
            Fields record = extra.Part(extra.Vint());
            const uint64_t type = record.Vint();
            if (!extra.ok() || !record.ok()) return ArchiveError::kDamaged;
            if (type == kEncryption) entry.encrypted = true;
            if (type == kHash && record.Vint() == 0) {
                const uint8_t* hash = record.Bytes(32);
                if (!record.ok()) return ArchiveError::kDamaged;
                std::copy(hash, hash + 32, item.blake.emplace().begin());
            }
            if (type == kRedirection) {
                constexpr uint64_t kFileCopy = 5;
                const uint64_t kind = record.Vint();
                record.Vint();
                const uint64_t size = record.Vint();
                const uint8_t* name = record.Bytes(size);
                if (!record.ok()) return ArchiveError::kDamaged;
                const auto source = by_name_.find(ForwardSlashes(std::string(reinterpret_cast<const char*>(name), size_t(size))));
                if (kind != kFileCopy) {
                    entry.link = true;
                } else if (source == by_name_.end() || entries_[source->second].size != entry.size || items_[source->second].copy_of) {
                    return ArchiveError::kDamaged;
                } else {
                    item.copy_of = source->second;
                }
            }
        }
        return ArchiveError::kNone;
    }

    // ---- data

    ArchiveError Expand() {
        if (!spool_.Create()) return ArchiveError::kUnreadable;
        // A window never has to reach back over more than there is.
        unpacker_.SetWindow(size_t(std::clamp<uint64_t>(total_, rar::Window::kSmallest, std::max<uint64_t>(dictionary_, rar::Window::kSmallest))));
        ArchiveError chain = ArchiveError::kNone; // of the files a solid one goes on from
        bool first = true;
        for (size_t i = 0; i < items_.size(); ++i) {
            Item& item = items_[i];
            if (entries_[i].directory || item.copy_of) continue;
            const bool solid = item.solid && !first;
            if (!item.stored && !solid) chain = ArchiveError::kNone;
            if (entries_[i].encrypted) item.error = ArchiveError::kUnsupported;
            else if (!item.stored && chain != ArchiveError::kNone) item.error = chain;
            else if (!item.stored && !item.method) item.error = ArchiveError::kUnsupported;
            else if (!item.stored && item.dictionary > kMostDictionary) item.error = ArchiveError::kTooLarge;
            else {
                bool written = true;
                item.error = Save(i, solid, written);
                if (!written) return ArchiveError::kUnreadable;
            }
            if (item.stored) continue;
            first = false;
            chain = item.error;
        }
        return ArchiveError::kNone;
    }

    ArchiveError Save(size_t index, bool solid, bool& written) {
        const Item& item = items_[index];
        const uint64_t size = entries_[index].size;
        std::ofstream out = spool_.Write(index);
        Verifier verifier(item);
        const Sink sink = [&](const uint8_t* data, size_t count) {
            verifier.Update(data, count);
            out.write(reinterpret_cast<const char*>(data), std::streamsize(count));
            return bool(out);
        };
        ArchiveError error = ArchiveError::kNone;
        if (item.stored) {
            error = item.packed == size ? CopyOut(item, sink) : ArchiveError::kDamaged;
        } else {
            rar::BitSource in(file_, item.offset, item.packed);
            rar::Output output(size, sink);
            error = unpacker_.Unpack(*item.method, solid, in, output);
        }
        out.close();
        written = bool(out);
        return error == ArchiveError::kNone ? verifier.Finish(size) : error;
    }

    ArchiveError CopyOut(const Item& item, const Sink& sink) {
        std::vector<uint8_t> buffer(1 << 18);
        for (uint64_t done = 0; done < item.packed;) {
            const size_t count = size_t(std::min<uint64_t>(buffer.size(), item.packed - done));
            if (!file_.ReadAt(item.offset + done, buffer.data(), count)) return ArchiveError::kUnreadable;
            if (!sink(buffer.data(), count)) return ArchiveError::kCancelled;
            done += count;
        }
        return ArchiveError::kNone;
    }

    FileReader file_;
    uint64_t size_ = 0, total_ = 0, dictionary_ = kDictionary4;
    ArchiveLimits limits_;
    std::vector<Item> items_;
    std::map<std::string, size_t> by_name_;
    rar::Unpacker unpacker_;
    ArchiveSpool spool_;
    ArchiveError error_ = ArchiveError::kNone;
    bool expanded_ = false;
};

} // namespace

std::unique_ptr<ArchiveReader> OpenRar(const fs::path& archive, const ArchiveLimits& limits, ArchiveError& error) {
    auto reader = std::make_unique<RarReader>();
    error = reader->Open(archive, limits);
    if (error != ArchiveError::kNone) return nullptr;
    return reader;
}

} // namespace jpt
