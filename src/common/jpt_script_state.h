// What a custom session's save keeps of its scripts: the nvram tables of Trespasser CE's script instances, by class and
// object, as values only. Never anything of the VM itself. Little endian, versioned, every count and size checked on read.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jpt::script {

inline constexpr uint32_t kStateVersion = 1;
// ScriptingReadme.txt: arrays keep 65536 elements and blobs 256 kB in a save; the rest is not saved.
inline constexpr size_t kArrayMax = 65536;
inline constexpr size_t kBlobMax = 256 * 1024;
inline constexpr size_t kStateMax = 16u << 20;
inline constexpr size_t kEntriesMax = 4096;
inline constexpr size_t kNameMax = 1024;

struct Value {
    enum Type : uint8_t { kNull, kInt, kFloat, kBool, kString, kBlob, kArray };
    Type type = kNull;
    int64_t i = 0;
    float f = 0.0f;
    bool b = false;
    std::string bytes;         // string or blob
    std::vector<Value> items;  // array: no blob, no array
};

struct Slot {
    std::string name;
    Value value;
};

enum class Role : uint8_t { kCurrent = 1, kLevelStart = 2 };

struct Entry {
    Role role = Role::kCurrent;
    uint8_t kind = 0; // jpt::script::Kind
    std::string class_name, object;
    std::vector<Slot> slots;
};

std::vector<uint8_t> EncodeState(const std::vector<Entry>& entries);
// 0 with the reason when the bytes are not a state this version reads whole.
bool DecodeState(const uint8_t* bytes, size_t size, std::vector<Entry>& entries, std::string& problem);

} // namespace jpt::script
