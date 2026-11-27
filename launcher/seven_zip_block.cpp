#include "seven_zip_block.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "third_party/lzma/Bra.h"
#include "third_party/lzma/Delta.h"
#include "third_party/lzma/Lzma2Dec.h"

namespace jpt {
namespace {

enum : UInt32 { kCopy = 0, kDelta = 3, kLzma2 = 0x21, kLzma = 0x30101, kX86 = 0x3030103, kPowerPc = 0x3030205, kArm = 0x3030501, kThumb = 0x3030701, kSparc = 0x3030805 };
constexpr size_t kInput = 1 << 18, kOutput = 1 << 20;

void* Allocate(ISzAllocPtr, size_t size) { return size ? std::malloc(size) : nullptr; }
void Release(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kAlloc = {Allocate, Release};

struct Chain {
    CSzFolder folder{};
    const Byte* properties = nullptr;
    const CSzCoderInfo& main() const { return folder.Coders[0]; }
    const CSzCoderInfo* filter() const { return folder.NumCoders == 2 ? &folder.Coders[1] : nullptr; }
    const Byte* of(const CSzCoderInfo& coder) const { return properties + coder.PropsOffset; }

    bool Read(const CSzAr& db, UInt32 block) {
        CSzData data;
        properties = data.Data = db.CodersData + db.FoCodersOffsets[block];
        data.Size = db.FoCodersOffsets[size_t(block) + 1] - db.FoCodersOffsets[block];
        return SzGetNextFolderItem(&folder, &data) == SZ_OK && !data.Size && folder.UnpackStream == db.FoToMainUnpackSizeIndex[block];
    }

    bool Streamable() const {
        const UInt32 method = main().MethodID;
        if (folder.NumPackStreams != 1 || folder.PackStreams[0] != 0 || main().NumStreams != 1) return false;
        if (method != kCopy && method != kLzma && method != kLzma2) return false;
        if (method == kLzma2 && main().PropsSize != 1) return false;
        if (folder.NumCoders == 1) return folder.NumBonds == 0;
        if (folder.NumCoders != 2 || folder.NumBonds != 1 || folder.Bonds[0].InIndex != 1 || folder.Bonds[0].OutIndex != 0 || filter()->NumStreams != 1) return false;
        switch (filter()->MethodID) {
        case kDelta: return filter()->PropsSize == 1;
        case kX86:
        case kPowerPc:
        case kArm:
        case kThumb:
        case kSparc: return filter()->PropsSize == 0;
        default: return false;
        }
    }
};

// The main coder: some of the packed bytes in, some of the unpacked ones out.
class Coder {
public:
    Coder() {
        LzmaDec_CONSTRUCT(&lzma_)
        Lzma2Dec_CONSTRUCT(&lzma2_)
    }
    ~Coder() {
        LzmaDec_Free(&lzma_, &kAlloc);
        Lzma2Dec_Free(&lzma2_, &kAlloc);
    }
    Coder(const Coder&) = delete;
    Coder& operator=(const Coder&) = delete;

    SRes Start(const Chain& chain) {
        method_ = chain.main().MethodID;
        SRes result = SZ_OK;
        if (method_ == kLzma && (result = LzmaDec_Allocate(&lzma_, chain.of(chain.main()), chain.main().PropsSize, &kAlloc)) == SZ_OK) LzmaDec_Init(&lzma_);
        if (method_ == kLzma2 && (result = Lzma2Dec_Allocate(&lzma2_, *chain.of(chain.main()), &kAlloc)) == SZ_OK) Lzma2Dec_Init(&lzma2_);
        return result;
    }

    SRes Run(Byte* out, SizeT& out_size, const Byte* in, SizeT& in_size) {
        ELzmaStatus status;
        if (method_ == kLzma) return LzmaDec_DecodeToBuf(&lzma_, out, &out_size, in, &in_size, LZMA_FINISH_ANY, &status);
        if (method_ == kLzma2) return Lzma2Dec_DecodeToBuf(&lzma2_, out, &out_size, in, &in_size, LZMA_FINISH_ANY, &status);
        out_size = in_size = std::min(out_size, in_size);
        std::memcpy(out, in, out_size);
        return SZ_OK;
    }

private:
    UInt32 method_ = kCopy;
    CLzmaDec lzma_;
    CLzma2Dec lzma2_;
};

class BlockFilter {
public:
    explicit BlockFilter(const Chain& chain) : method_(chain.filter() ? chain.filter()->MethodID : UInt32(kCopy)) {
        if (method_ == kDelta) delta_ = *chain.of(*chain.filter()) + 1u;
        Delta_Init(state_);
    }

    // How many bytes are done; the rest is wanted again with what follows it.
    size_t Run(Byte* data, size_t size) {
        Byte* end = data + size;
        switch (method_) {
        case kDelta: Delta_Decode(state_, delta_, data, size); break;
        case kX86: end = Z7_BRANCH_CONV_ST_DEC(X86)(data, size, at_, &x86_); break;
        case kPowerPc: end = Z7_BRANCH_CONV_DEC(PPC)(data, size, at_); break;
        case kArm: end = Z7_BRANCH_CONV_DEC(ARM)(data, size, at_); break;
        case kThumb: end = Z7_BRANCH_CONV_DEC(ARMT)(data, size, at_); break;
        case kSparc: end = Z7_BRANCH_CONV_DEC(SPARC)(data, size, at_); break;
        default: break;
        }
        at_ += UInt32(end - data);
        return size_t(end - data);
    }

private:
    UInt32 method_, at_ = 0, x86_ = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
    unsigned delta_ = 1;
    Byte state_[DELTA_STATE_SIZE];
};

ArchiveError ErrorOf(SRes result) {
    return result == SZ_ERROR_MEM ? ArchiveError::kOutOfMemory : result == SZ_ERROR_UNSUPPORTED ? ArchiveError::kUnsupported : ArchiveError::kDamaged;
}

} // namespace

bool IsStreamableBlock(const CSzArEx& archive, UInt32 block) {
    Chain chain;
    return chain.Read(archive.db, block) && chain.Streamable();
}

ArchiveError StreamBlock(const CSzArEx& archive, UInt32 block, FileReader& file, const ArchiveReader::Sink& sink) {
    const CSzAr& db = archive.db;
    Chain chain;
    if (!chain.Read(db, block) || !chain.Streamable()) return ArchiveError::kUnsupported;
    Coder coder;
    if (const SRes started = coder.Start(chain); started != SZ_OK) return ErrorOf(started);
    BlockFilter filter(chain);
    const UInt32 pack = db.FoStartPackStreamIndex[block];
    uint64_t from = archive.dataPos + db.PackPositions[pack], packed = db.PackPositions[size_t(pack) + 1] - db.PackPositions[pack];
    uint64_t unpacked = SzAr_GetFolderUnpackSize(&db, block);
    std::vector<Byte> in(kInput), out(kOutput);
    size_t in_at = 0, in_size = 0, kept = 0;
    while (unpacked) {
        if (in_at == in_size) {
            in_at = 0;
            in_size = size_t(std::min<uint64_t>(in.size(), packed));
            if (in_size && !file.ReadAt(from, in.data(), in_size)) return ArchiveError::kUnreadable;
            from += in_size;
            packed -= in_size;
        }
        SizeT produced = size_t(std::min<uint64_t>(out.size() - kept, unpacked)), consumed = in_size - in_at;
        if (const SRes result = coder.Run(&out[kept], produced, in.data() + in_at, consumed); result != SZ_OK) return ErrorOf(result);
        if (!produced && !consumed) return ArchiveError::kDamaged;
        in_at += consumed;
        unpacked -= produced;
        const size_t ready = kept + produced, done = filter.Run(out.data(), ready), send = unpacked ? done : ready;
        if (send && !sink(out.data(), send)) return ArchiveError::kCancelled;
        kept = ready - send;
        std::memmove(out.data(), out.data() + send, kept);
    }
    return ArchiveError::kNone;
}

} // namespace jpt
