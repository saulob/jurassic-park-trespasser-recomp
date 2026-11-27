#include "jpt_script_state.h"

#include <cstring>

namespace jpt::script {
namespace {

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
}

void Put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int k = 0; k < 8; ++k) out.push_back(static_cast<uint8_t>(v >> (8 * k)));
}

void PutText(std::vector<uint8_t>& out, const std::string& text) {
    Put32(out, static_cast<uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void PutFloat(std::vector<uint8_t>& out, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Put32(out, bits);
}

void PutValue(std::vector<uint8_t>& out, const Value& v) {
    out.push_back(v.type);
    switch (v.type) {
    case Value::kNull: break;
    case Value::kVector3:
        for (int k = 0; k < 3; ++k) PutFloat(out, v.v[k]);
        break;
    case Value::kRotate3:
        for (int k = 0; k < 4; ++k) PutFloat(out, v.v[k]);
        break;
    case Value::kInstance: Put32(out, static_cast<uint32_t>(v.i)); break;
    case Value::kInt: Put64(out, static_cast<uint64_t>(v.i)); break;
    case Value::kFloat: PutFloat(out, v.f); break;
    case Value::kBool: out.push_back(v.b ? 1 : 0); break;
    case Value::kString:
    case Value::kBlob: PutText(out, v.bytes); break;
    case Value::kArray:
        Put32(out, static_cast<uint32_t>(v.items.size()));
        for (const Value& item : v.items) PutValue(out, item);
        break;
    }
}

class Reader {
public:
    Reader(const uint8_t* bytes, size_t size) : p_(bytes), left_(size) {}

    uint32_t version = kStateVersion;

    bool U8(uint8_t& v) {
        if (left_ < 1) return false;
        v = *p_++;
        --left_;
        return true;
    }

    bool U32(uint32_t& v) {
        if (left_ < 4) return false;
        v = static_cast<uint32_t>(p_[0]) | static_cast<uint32_t>(p_[1]) << 8 | static_cast<uint32_t>(p_[2]) << 16 | static_cast<uint32_t>(p_[3]) << 24;
        p_ += 4;
        left_ -= 4;
        return true;
    }

    bool U64(uint64_t& v) {
        uint32_t lo, hi;
        if (!U32(lo) || !U32(hi)) return false;
        v = static_cast<uint64_t>(hi) << 32 | lo;
        return true;
    }

    bool F32(float& out) {
        uint32_t bits;
        if (!U32(bits)) return false;
        std::memcpy(&out, &bits, 4);
        return true;
    }

    bool Text(std::string& out, size_t max) {
        uint32_t n;
        if (!U32(n) || n > max || n > left_) return false;
        out.assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        left_ -= n;
        return true;
    }

    bool ReadValue(Value& v, bool in_array) {
        uint8_t type;
        if (!U8(type)) return false;
        if (type > (version >= 2 ? Value::kInstance : Value::kArray)) return false;
        v.type = static_cast<Value::Type>(type);
        switch (v.type) {
        case Value::kNull: return true;
        case Value::kInt: {
            uint64_t bits;
            if (!U64(bits)) return false;
            v.i = static_cast<int64_t>(bits);
            return true;
        }
        case Value::kFloat: return F32(v.f);
        case Value::kVector3: return F32(v.v[0]) && F32(v.v[1]) && F32(v.v[2]);
        case Value::kRotate3: return F32(v.v[0]) && F32(v.v[1]) && F32(v.v[2]) && F32(v.v[3]);
        case Value::kInstance: {
            uint32_t handle;
            if (!U32(handle)) return false;
            v.i = handle;
            return true;
        }
        case Value::kBool: {
            uint8_t b;
            if (!U8(b) || b > 1) return false;
            v.b = b != 0;
            return true;
        }
        case Value::kString: return Text(v.bytes, kStateMax);
        case Value::kBlob: return !in_array && Text(v.bytes, kBlobMax);
        case Value::kArray: {
            uint32_t count;
            if (in_array || !U32(count) || count > kArrayMax || count > left_) return false;
            v.items.resize(count);
            for (Value& item : v.items)
                if (!ReadValue(item, true)) return false;
            return true;
        }
        }
        return false;
    }

    size_t left() const { return left_; }

private:
    const uint8_t* p_;
    size_t left_;
};

} // namespace

std::vector<uint8_t> EncodeState(const std::vector<Entry>& entries) {
    std::vector<uint8_t> out;
    Put32(out, kStateVersion);
    Put32(out, static_cast<uint32_t>(entries.size()));
    for (const Entry& e : entries) {
        out.push_back(static_cast<uint8_t>(e.role));
        out.push_back(e.kind);
        PutText(out, e.class_name);
        PutText(out, e.object);
        out.push_back(e.flags);
        Put32(out, static_cast<uint32_t>(e.slots.size()));
        for (const Slot& s : e.slots) {
            PutText(out, s.name);
            PutValue(out, s.value);
        }
    }
    return out;
}

bool DecodeState(const uint8_t* bytes, size_t size, std::vector<Entry>& entries, std::string& problem) {
    Reader r(bytes, size);
    uint32_t version, count;
    entries.clear();
    if (size > kStateMax) return problem = "it is larger than " + std::to_string(kStateMax) + " bytes", false;
    if (!r.U32(version) || !r.U32(count)) return problem = "it is truncated", false;
    if (version != 1 && version != kStateVersion) return problem = "its version " + std::to_string(version) + " is not known", false;
    r.version = version;
    if (count > kEntriesMax || count > r.left()) return problem = "it counts " + std::to_string(count) + " scripts", false;
    entries.resize(count);
    for (Entry& e : entries) {
        uint8_t role, kind;
        uint32_t slots;
        if (!r.U8(role) || !r.U8(kind) || !r.Text(e.class_name, kNameMax) || !r.Text(e.object, kNameMax) || (version >= 2 && !r.U8(e.flags)) ||
            !r.U32(slots))
            return entries.clear(), problem = "it is truncated", false;
        if ((role != static_cast<uint8_t>(Role::kCurrent) && role != static_cast<uint8_t>(Role::kLevelStart)) || e.class_name.empty())
            return entries.clear(), problem = "an entry is malformed", false;
        if (slots > kArrayMax || slots > r.left()) return entries.clear(), problem = "an entry counts " + std::to_string(slots) + " slots", false;
        e.role = static_cast<Role>(role);
        e.kind = kind;
        e.slots.resize(slots);
        for (Slot& s : e.slots)
            if (!r.Text(s.name, kNameMax) || !r.ReadValue(s.value, false)) return entries.clear(), problem = "a slot is truncated or malformed", false;
    }
    if (r.left()) return entries.clear(), problem = std::to_string(r.left()) + " bytes follow its last entry", false;
    return true;
}

} // namespace jpt::script
