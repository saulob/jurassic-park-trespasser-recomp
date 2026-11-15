// Developer tool: reads the English Stream.tpa fix (a development source, never shipped or read by the port) next to the
// player's retail Stream.tpa and derives what the port embeds.
//   stream_fix_pack report   <retail Stream.tpa> <fixed Stream.tpa>
//   stream_fix_pack generate <retail Stream.tpa> <fixed Stream.tpa> <assets folder>
// generate writes assets/translations/en.subtitles.txt (the canonical English captions and timings of every sample the
// retail audio or the restored audio can play) and assets/restored_audio (the audio records the retail file lacks).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "jpt_hash.h"
#include "jpt_l10n.h"

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kFixedSize = 223761342;
constexpr const char* kFixedSha256 = "f77abaee8d91d5c091781e352d56f96eaf7968f5af240f03043d863907c34397";
constexpr uint32_t kRetailRecordHeader = 0x24;

struct RestoredSample {
    const char* name;
};

// Samples the retail levels play that the retail Stream.tpa lacks (or holds in a short version without the dialogue).
constexpr RestoredSample kRestored[] = {{"VA208"}, {"VA34"}, {"VA24"}, {"SPEC-TOUR GUIDE"}, {"VCRAYCOMPUTER"}};

uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

void PutLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

struct Sample {
    uint32_t id = 0;
    uint8_t entry[JPT_TPA_ENTRY] = {};
    uint32_t offset = 0, size = 0, header = 0;
    uint32_t Field(int i) const { return Le32(entry + 4 * i); }
    uint32_t Format() const { return Field(12); }
    uint32_t Rate() const { return Field(11); }
    uint32_t DecodedBytes() const { return Field(10); }
    uint32_t PayloadBytes() const { return size - header; }
    int Channels() const { return Format() & 0x200 ? 2 : 1; }
    bool Compressed() const { return Format() & 0x10000; }
    double Seconds() const { return double(DecodedBytes()) / (double(Rate()) * 2.0 * Channels()); }
};

struct Tpa {
    fs::path path;
    uint64_t size = 0;
    std::vector<Sample> samples;
    std::map<uint32_t, size_t> first;

    bool Load(const fs::path& file) {
        path = file;
        std::error_code ec;
        size = fs::file_size(file, ec);
        uint8_t head[JPT_TPA_HEADER];
        if (ec || !Read(0, head, sizeof head) || Le32(head) != JPT_TPA_MAGIC || Le32(head + 8) != JPT_TPA_HEADER) return false;
        const uint32_t count = Le32(head + 4);
        std::vector<uint8_t> dir(size_t(count) * JPT_TPA_ENTRY);
        if (!count || count > JPT_TPA_MAX_SAMPLES || !Read(JPT_TPA_HEADER, dir.data(), dir.size())) return false;
        for (uint32_t i = 0; i < count; ++i) {
            Sample s;
            std::memcpy(s.entry, dir.data() + size_t(i) * JPT_TPA_ENTRY, JPT_TPA_ENTRY);
            s.id = Le32(s.entry);
            s.offset = Le32(s.entry + 4);
            s.size = Le32(s.entry + 8);
            s.header = Le32(s.entry + 0x1C);
            if (s.header < kRetailRecordHeader || s.header > s.size || uint64_t(s.offset) + s.size > size) return false;
            first.emplace(s.id, samples.size());
            samples.push_back(s);
        }
        return true;
    }

    bool Read(uint64_t offset, void* buf, size_t n) const {
        std::ifstream in(path, std::ios::binary);
        in.seekg(std::streamoff(offset));
        in.read(static_cast<char*>(buf), std::streamsize(n));
        return in && size_t(in.gcount()) == n;
    }

    std::vector<uint8_t> Record(const Sample& s) const {
        std::vector<uint8_t> out(s.size);
        if (!Read(s.offset, out.data(), out.size())) out.clear();
        return out;
    }

    const Sample* Find(uint32_t id) const {
        const auto it = first.find(id);
        return it == first.end() ? nullptr : &samples[it->second];
    }

    bool IsFirst(const Sample& s) const { return &samples[first.at(s.id)] == &s; }
};

struct Line {
    uint32_t units;
    std::string text; // cp1252 bytes
};

std::vector<Line> CaptionLines(const std::vector<uint8_t>& record, const Sample& s) {
    std::vector<Line> lines;
    if (s.header <= kRetailRecordHeader || record.size() < s.header) return lines;
    const uint32_t block = Le32(record.data() + kRetailRecordHeader);
    if (block > s.header - kRetailRecordHeader) return lines;
    jpt_caption_lines(record.data() + kRetailRecordHeader, block,
                      [](void* ctx, uint32_t units, const char* text, size_t n) {
                          static_cast<std::vector<Line>*>(ctx)->push_back({units, std::string(text, n)});
                      },
                      &lines);
    return lines;
}

bool SameLines(const std::vector<Line>& a, const std::vector<Line>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Line& x, const Line& y) {
               return x.units == y.units && x.text == y.text;
           });
}

std::vector<uint8_t> PayloadOf(const std::vector<uint8_t>& record, const Sample& s) {
    return std::vector<uint8_t>(record.begin() + s.header, record.end());
}

std::string Sha(const std::vector<uint8_t>& bytes) {
    char hex[65];
    jpt_sha256_buffer_hex(bytes.data(), bytes.size(), hex);
    return hex;
}

std::string Hex8(uint32_t v) {
    char text[16];
    std::snprintf(text, sizeof text, "%08x", v);
    return text;
}

const int kStepTable[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
                            157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
                            1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
                            10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
constexpr size_t kAdpcmBlock = 1028;

// The engine's ADPCM (0x0043DDC0): blocks of a 16-bit predictor, a step index and 1024 bytes of nibbles, low nibble first.
std::vector<int16_t> DecodeAdpcm(const std::vector<uint8_t>& data) {
    std::vector<int16_t> out;
    for (size_t at = 0; at + 3 < data.size(); at += kAdpcmBlock) {
        const size_t n = std::min(kAdpcmBlock, data.size() - at);
        int pred = int16_t(data[at] | data[at + 1] << 8), index = std::min<int>(data[at + 2], 88);
        if (at == 0) out.push_back(int16_t(pred));
        for (size_t i = 4; i < n; ++i) {
            for (int half = 0; half < 2; ++half) {
                const int nibble = half ? data[at + i] >> 4 : data[at + i] & 15, step = kStepTable[index];
                int delta = step >> 3;
                if (nibble & 4) delta += step;
                if (nibble & 2) delta += step >> 1;
                if (nibble & 1) delta += step >> 2;
                pred = std::clamp(pred + (nibble & 8 ? -delta : delta), -32768, 32767);
                out.push_back(int16_t(pred));
                index = std::clamp(index + kIndexTable[nibble & 7], 0, 88);
            }
        }
    }
    return out;
}

bool SameAudioReencoded(const std::vector<uint8_t>& retail_payload, const Sample& retail, const std::vector<uint8_t>& fixed_payload,
                        const Sample& fixed) {
    if (!retail.Compressed() || fixed.Compressed() || retail.Channels() != 1 || fixed.Channels() != 1) return false;
    const std::vector<int16_t> decoded = DecodeAdpcm(retail_payload);
    const size_t n = std::min(decoded.size(), fixed_payload.size() / 2);
    size_t equal = 0;
    for (size_t i = 0; i < n; ++i) equal += decoded[i] == int16_t(fixed_payload[2 * i] | fixed_payload[2 * i + 1] << 8);
    return n && equal * 1000 >= n * 995;
}

std::string Escaped(const std::string& text) {
    std::string out(text.size() * 2 + 1, '\0');
    const size_t n = jpt_l10n_escape(text.c_str(), out.data(), out.size());
    out.resize(n == size_t(-1) ? 0 : n);
    return out;
}

std::string Utf8(const std::string& cp1252) {
    std::string out(cp1252.size() * 3 + 1, '\0');
    const size_t n = jpt_cp_to_utf8(1252, cp1252.data(), cp1252.size(), out.data(), out.size());
    out.resize(n == size_t(-1) ? 0 : n);
    return out;
}

std::string Squeezed(const std::string& text) {
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\r' && c != '\n') out += c;
    return out;
}

// The retail text stays where the fix only changed blanks or line breaks; its two typos are not carried over.
std::vector<Line> CanonicalLines(uint32_t id, const std::vector<Line>& retail, const std::vector<Line>& fixed) {
    if (retail.size() != fixed.size()) return fixed;
    std::vector<Line> out = fixed;
    for (size_t i = 0; i < out.size(); ++i) {
        const bool typo = id == 0x3324f842 && i == 0;
        if (typo || Squeezed(retail[i].text) == Squeezed(fixed[i].text)) out[i].text = retail[i].text;
    }
    return out;
}

struct Delta {
    std::vector<uint32_t> identical, caption_only, descriptor_only, reencoded, reencoded_layout, different_audio, added;
    std::vector<uint32_t> retail_only, duplicate_ids;
    std::map<uint32_t, std::string> retail_payload_sha;
};

std::string Name(uint32_t id) {
    for (const RestoredSample& r : kRestored)
        if (jpt_sample_id(r.name) == id) return r.name;
    return {};
}

std::string Describe(const Sample& s) {
    char text[160];
    std::snprintf(text, sizeof text, "%s  %u bytes  %.3f s  %u Hz  %s  format 0x%x", Hex8(s.id).c_str(), s.PayloadBytes(), s.Seconds(), s.Rate(),
                  s.Compressed() ? "ADPCM" : "PCM", s.Format());
    return text;
}

Delta Compare(const Tpa& retail, const Tpa& fixed) {
    Delta d;
    for (const Sample& f : fixed.samples) {
        if (!fixed.IsFirst(f)) {
            d.duplicate_ids.push_back(f.id);
            continue;
        }
        const Sample* r = retail.Find(f.id);
        if (!r) {
            d.added.push_back(f.id);
            continue;
        }
        const std::vector<uint8_t> rr = retail.Record(*r), fr = fixed.Record(f);
        const std::vector<uint8_t> rp = PayloadOf(rr, *r), fp = PayloadOf(fr, f);
        d.retail_payload_sha[f.id] = Sha(rp);
        if (rp == fp) {
            const bool same_caption = SameLines(CaptionLines(rr, *r), CaptionLines(fr, f));
            const bool same_descriptor = std::equal(rr.begin(), rr.begin() + 8, fr.begin()) &&
                                         std::equal(rr.begin() + 12, rr.begin() + kRetailRecordHeader, fr.begin() + 12);
            (same_caption && same_descriptor ? d.identical : same_descriptor ? d.caption_only : d.descriptor_only).push_back(f.id);
        } else if (SameAudioReencoded(rp, *r, fp, f)) {
            d.reencoded.push_back(f.id);
        } else if (std::abs(r->Seconds() - f.Seconds()) <= 0.03 * r->Seconds() + 0.1) {
            d.reencoded_layout.push_back(f.id);
        } else {
            d.different_audio.push_back(f.id);
        }
    }
    for (const Sample& r : retail.samples)
        if (!fixed.Find(r.id)) d.retail_only.push_back(r.id);
    return d;
}

bool Reachable(const Tpa& retail, uint32_t id) {
    if (retail.Find(id)) return true;
    for (const RestoredSample& r : kRestored)
        if (jpt_sample_id(r.name) == id) return true;
    return false;
}

void Report(const Tpa& retail, const Tpa& fixed) {
    const Delta d = Compare(retail, fixed);
    auto captioned = [](const Tpa& t) {
        int n = 0;
        for (const Sample& s : t.samples) n += s.header > kRetailRecordHeader;
        return n;
    };
    std::printf("retail Stream.tpa: %llu bytes, %zu directory entries, %zu IDs, %d captioned\n", (unsigned long long)retail.size, retail.samples.size(),
                retail.first.size(), captioned(retail));
    std::printf("fixed  Stream.tpa: %llu bytes, %zu directory entries, %zu IDs, %d captioned, %zu repeated IDs\n", (unsigned long long)fixed.size,
                fixed.samples.size(), fixed.first.size(), captioned(fixed), d.duplicate_ids.size());
    std::printf("same bytes and caption: %zu\n", d.identical.size());
    std::printf("same audio bytes, caption/timing changed: %zu\n", d.caption_only.size());
    std::printf("same audio bytes, descriptor changed: %zu\n", d.descriptor_only.size());
    std::printf("audio re-encoded (retail ADPCM decodes to the fixed PCM): %zu\n", d.reencoded.size());
    std::printf("audio re-encoded, channel layout or rate field differs, same length: %zu\n", d.reencoded_layout.size());
    std::printf("audio different (not the same recording or length): %zu\n", d.different_audio.size());
    std::printf("new IDs (absent from the retail directory): %zu\n", d.added.size());
    std::printf("retail IDs missing from the fixed file: %zu\n", d.retail_only.size());

    uint64_t added_payload = 0;
    for (uint32_t id : d.added) added_payload += fixed.Find(id)->PayloadBytes();
    std::printf("payload of all new IDs: %llu bytes\n", (unsigned long long)added_payload);

    std::printf("\nsamples with different audio:\n");
    for (uint32_t id : d.different_audio) {
        const Sample *r = retail.Find(id), *f = fixed.Find(id);
        std::printf("  %s %s\n    retail %s\n    fixed  %s\n", Hex8(id).c_str(), Name(id).c_str(), Describe(*r).c_str(), Describe(*f).c_str());
    }

    std::printf("\nnew IDs (captions from the fixed file; * = embedded in the port):\n");
    for (uint32_t id : d.added) {
        const Sample& f = *fixed.Find(id);
        const std::vector<Line> lines = CaptionLines(fixed.Record(f), f);
        std::printf("  %c %s %-18s %s", Reachable(retail, id) ? '*' : ' ', Hex8(id).c_str(), Name(id).c_str(), Describe(f).c_str());
        uint32_t units = 0;
        for (const Line& l : lines) units += l.units;
        std::printf("  %zu lines %.1f s%s\n", lines.size(), units * 0.2, lines.empty() ? " (no caption)" : "");
        for (const Line& l : lines) std::printf("        %3u  %s\n", l.units, Utf8(l.text).c_str());
    }

    std::printf("\nretail IDs missing from the fixed file:");
    for (uint32_t id : d.retail_only) std::printf(" %s", Hex8(id).c_str());
    std::printf("\n");
}

bool WriteFile(const fs::path& path, const void* data, size_t n) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), std::streamsize(n));
    return bool(out);
}

bool Generate(const Tpa& retail, const Tpa& fixed, const fs::path& assets) {
    const Delta d = Compare(retail, fixed);
    const fs::path audio_dir = assets / "restored_audio", translations = assets / "translations";
    std::error_code ec;
    fs::create_directories(audio_dir, ec);
    fs::create_directories(translations, ec);
    for (const auto& entry : fs::directory_iterator(audio_dir, ec)) fs::remove(entry.path(), ec);

    std::string manifest = std::string("# Trespasser Recompiled restored audio 1\n");
    uint64_t payload_total = 0, record_total = 0;
    std::set<uint32_t> embedded;
    for (const RestoredSample& r : kRestored) {
        const uint32_t id = jpt_sample_id(r.name);
        const Sample* f = fixed.Find(id);
        if (!f || !fixed.IsFirst(*f)) {
            std::fprintf(stderr, "%s (%s) is not in the fixed file\n", r.name, Hex8(id).c_str());
            return false;
        }
        const std::vector<uint8_t> record = fixed.Record(*f);
        const std::vector<uint8_t> payload = PayloadOf(record, *f);
        const Sample* rs = retail.Find(id);
        std::string replaces = "-";
        if (rs) replaces = d.retail_payload_sha.count(id) ? d.retail_payload_sha.at(id) : Sha(PayloadOf(retail.Record(*rs), *rs));
        if (f->Compressed() || f->Channels() > 2) {
            std::fprintf(stderr, "%s: unexpected format 0x%x\n", r.name, f->Format());
            return false;
        }
        const std::string file = Hex8(id) + ".sample";
        if (!WriteFile(audio_dir / file, record.data(), record.size())) return false;
        char line[640];
        std::snprintf(line, sizeof line,
                      "sample\t%s\tname=%s\tfile=%s\trecord=%zu\tpayload=%zu\tsha256=%s\tformat=0x%x\tcodec=pcm\tchannels=%d\tbits=16\trate=%u\t"
                      "duration_ms=%u\treplaces=%s\n",
                      Hex8(id).c_str(), r.name, file.c_str(), record.size(), payload.size(), Sha(payload).c_str(), f->Format(), f->Channels(), f->Rate(),
                      unsigned(double(f->DecodedBytes()) * 1000.0 / (double(f->Rate()) * 2.0 * f->Channels()) + 0.5), replaces.c_str());
        manifest += line;
        payload_total += payload.size();
        record_total += record.size();
        embedded.insert(id);
        std::printf("embedded %s %-16s %zu bytes (%s)\n", Hex8(id).c_str(), r.name, record.size(), rs ? "replaces the retail sample" : "new");
    }
    if (!WriteFile(audio_dir / "restored_audio.manifest.txt", manifest.data(), manifest.size())) return false;

    std::string subtitles = std::string(JPT_L10N_SUBTITLES_HEADER) + "\n";
    int samples = 0, changed = 0;
    for (const Sample& f : fixed.samples) {
        if (!fixed.IsFirst(f) || f.header <= kRetailRecordHeader || !Reachable(retail, f.id)) continue;
        const std::vector<Line> fixed_lines = CaptionLines(fixed.Record(f), f);
        const Sample* r = retail.Find(f.id);
        const std::vector<Line> retail_lines = r ? CaptionLines(retail.Record(*r), *r) : std::vector<Line>();
        const std::vector<Line> lines = r && !retail_lines.empty() ? CanonicalLines(f.id, retail_lines, fixed_lines) : fixed_lines;
        subtitles += "S\tstream\t" + Hex8(f.id) + "\n";
        for (const Line& l : lines) subtitles += "C\t" + std::to_string(l.units) + "\t" + Escaped(Utf8(l.text)) + "\n";
        ++samples;
        changed += !SameLines(lines, retail_lines);
    }
    if (!WriteFile(translations / "en.subtitles.txt", subtitles.data(), subtitles.size())) return false;
    std::printf("en.subtitles.txt: %d samples (%d differ from the retail captions), %zu bytes\n", samples, changed, subtitles.size());
    std::printf("restored audio: %zu samples, %llu payload bytes, %llu record bytes, manifest %zu bytes\n", embedded.size(),
                (unsigned long long)payload_total, (unsigned long long)record_total, manifest.size());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const bool report = argc == 4 && !std::strcmp(argv[1], "report"), generate = argc == 5 && !std::strcmp(argv[1], "generate");
    if (!report && !generate) {
        std::fprintf(stderr, "usage: stream_fix_pack report|generate <retail Stream.tpa> <fixed Stream.tpa> [<assets folder>]\n");
        return 2;
    }
    Tpa retail, fixed;
    if (!retail.Load(argv[2]) || !fixed.Load(argv[3])) {
        std::fprintf(stderr, "cannot read a TPA\n");
        return 1;
    }
    if (fixed.size != kFixedSize) {
        std::fprintf(stderr, "the fixed Stream.tpa is not the known v1.0.0 file (size)\n");
        return 1;
    }
    {
        std::ifstream in(argv[3], std::ios::binary);
        jpt_sha256_t sha;
        jpt_sha256_init(&sha);
        std::vector<char> block(1 << 20);
        while (in.read(block.data(), std::streamsize(block.size())) || in.gcount()) jpt_sha256_update(&sha, block.data(), size_t(in.gcount()));
        char hex[65];
        jpt_sha256_hex(&sha, hex);
        if (std::strcmp(hex, kFixedSha256)) {
            std::fprintf(stderr, "the fixed Stream.tpa is not the known v1.0.0 file (SHA-256)\n");
            return 1;
        }
    }
    if (report) {
        Report(retail, fixed);
        return 0;
    }
    return Generate(retail, fixed, argv[4]) ? 0 : 1;
}
