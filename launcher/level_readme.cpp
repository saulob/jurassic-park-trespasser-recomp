#include "level_readme.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>

#include "level_ini.h"

namespace jpt {
namespace {

constexpr size_t kTextLimit = 256 * 1024;
constexpr size_t kSentenceTokens = 80;
constexpr size_t kObjectTokens = 20;
constexpr size_t kTargetTokens = 8;
constexpr size_t kQuoteLength = 160;

struct Token {
    std::string raw;  // without the punctuation around it
    std::string text; // raw in lower case, without surrounding slashes
    bool slashed = false;
};
using Sentence = std::vector<Token>;

template <size_t N>
bool OneOf(const std::string& text, const char* const (&list)[N]) {
    return std::any_of(std::begin(list), std::end(list), [&text](const char* item) { return text == item; });
}

std::string LastSegment(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string Strip(const std::string& text, const char* around) {
    const size_t first = text.find_first_not_of(around);
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(around) - first + 1);
}

// Curly quotes become plain ones; anything else past ASCII stays.
std::string PlainQuotes(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, 2, "\xE2\x80") == 0 && i + 2 < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text[i + 2]);
            if (c >= 0x98 && c <= 0x9F) {
                out += c < 0x9C ? '\'' : '"';
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::vector<Sentence> Sentences(const std::string& source) {
    constexpr char kAround[] = "\"'`*()[]{}<>,;:!?|=~";
    const std::string text = PlainQuotes(source.substr(0, kTextLimit));
    std::vector<Sentence> sentences(1);
    const auto end_sentence = [&sentences] {
        if (!sentences.back().empty()) sentences.emplace_back();
    };
    size_t at = 0;
    while (at < text.size()) {
        int lines = 0;
        for (; at < text.size() && std::isspace(static_cast<unsigned char>(text[at])); ++at) lines += text[at] == '\n';
        if (lines > 1) end_sentence();
        size_t stop = at;
        while (stop < text.size() && !std::isspace(static_cast<unsigned char>(text[stop]))) ++stop;
        if (stop == at) break;
        std::string word = text.substr(at, stop - at);
        at = stop;
        const std::string quoted = Strip(word, "\"'`*)]>");
        const bool ends = !quoted.empty() && (quoted.back() == '.' || quoted.back() == '!' || quoted.back() == '?');
        Token token;
        token.raw = Strip(Strip(word, kAround), kAround);
        while (!token.raw.empty() && token.raw.back() == '.') token.raw.pop_back();
        token.raw = Strip(token.raw, kAround);
        token.slashed = token.raw.find_first_of("/\\") != std::string::npos;
        token.text = LowerAscii(Strip(token.raw, "/\\"));
        if (!token.text.empty()) sentences.back().push_back(std::move(token));
        if (ends || sentences.back().size() == kSentenceTokens) end_sentence();
    }
    if (sentences.back().empty()) sentences.pop_back();
    return sentences;
}

std::string Quote(const Sentence& sentence) {
    std::string text;
    for (const Token& token : sentence) text += (text.empty() ? "" : " ") + token.raw;
    if (text.size() <= kQuoteLength) return text;
    size_t cut = kQuoteLength;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "...";
}

bool Has(const Sentence& sentence, const char* word) {
    return std::any_of(sentence.begin(), sentence.end(), [word](const Token& t) { return t.text == word; });
}

bool IsVerb(const Token& token) {
    static constexpr const char* kVerbs[] = {"place", "put", "move", "copy", "extract", "unzip", "unpack", "drop"};
    // "1.Download", "a.Find": a list mark written against its word.
    const size_t dot = token.text.rfind('.');
    return OneOf(token.text, kVerbs) || (dot != std::string::npos && dot < 3 && OneOf(token.text.substr(dot + 1), kVerbs));
}

bool IsInto(const std::string& text) {
    static constexpr const char* kWords[] = {"in", "into", "to", "inside", "within"};
    return OneOf(text, kWords);
}

bool IsFolderWord(const std::string& text) {
    static constexpr const char* kWords[] = {"directory", "directories", "folder", "folders", "dir", "subfolder", "subdirectory"};
    return OneOf(text, kWords);
}

bool TargetNamed(const std::string& name, ReadmeTarget& target) {
    static const std::pair<const char*, ReadmeTarget> kNames[] = {
        {"data", ReadmeTarget::kData}, {"anim", ReadmeTarget::kAnim},   {"menu", ReadmeTarget::kMenu},   {"texhw", ReadmeTarget::kTexhw},
        {"maps", ReadmeTarget::kMaps}, {"mods", ReadmeTarget::kModRoot}, {"mod", ReadmeTarget::kModRoot}, {"fms", ReadmeTarget::kModRoot}};
    for (const auto& [known, value] : kNames)
        if (name == known) return target = value, true;
    return false;
}

struct Place {
    bool found = false;   // a destination was named
    bool known = false;   // and it is one of the folders above
    ReadmeTarget target = ReadmeTarget::kData;
};

Place PlaceAfter(const Sentence& sentence, size_t into) {
    Place place;
    for (size_t i = into + 1; i < sentence.size() && i <= into + kTargetTokens; ++i) {
        const Token& token = sentence[i];
        if (IsInto(token.text) && i > into + 1) break;
        const bool folder_word = IsFolderWord(token.text);
        const bool before_folder = i + 1 < sentence.size() && IsFolderWord(sentence[i + 1].text);
        ReadmeTarget target = ReadmeTarget::kData;
        if ((before_folder || token.slashed) && TargetNamed(LowerAscii(LastSegment(token.text)), target)) {
            place = {true, true, target};
            return place;
        }
        if (folder_word || token.slashed) {
            place.found = true;
            return place;
        }
    }
    return place;
}

bool LooksLikeFile(const std::string& text) {
    static constexpr const char* kTypes[] = {"scn", "grf", "wtd", "pid", "swp", "spz", "spp", "asa", "asb", "tpa", "sky", "ddf",
                                             "smk", "bmp", "tga", "dds", "png", "ini"};
    const size_t dot = text.rfind('.');
    return dot != std::string::npos && dot > 0 && OneOf(text.substr(dot + 1), kTypes);
}

bool FitsTarget(ReadmeTarget target, const std::string& file) {
    static constexpr const char* kPictures[] = {"bmp", "tga", "png", "jpg", "pcx", "gif"};
    static constexpr const char* kTextures[] = {"dds", "tga", "bmp"};
    const std::string type = LowerAscii(file.substr(file.rfind('.') + 1));
    switch (target) {
    case ReadmeTarget::kAnim: return type == "asa";
    case ReadmeTarget::kMaps: return OneOf(type, kPictures);
    case ReadmeTarget::kTexhw: return OneOf(type, kTextures);
    case ReadmeTarget::kMenu: return OneOf(type, kPictures) || type == "ddf" || type == "smk";
    case ReadmeTarget::kData: return type != "ini";
    case ReadmeTarget::kModRoot: break;
    }
    return false;
}

class Reader {
public:
    Reader(const std::vector<std::string>& paths, const std::vector<std::string>& folders, ReadmeNotes& notes)
        : paths_(paths), folders_(folders), notes_(notes) {}

    void Read(const Sentence& sentence) {
        Dependencies(sentence);
        Configs(sentence);
        for (size_t verb = 0; verb < sentence.size(); ++verb) {
            if (!IsVerb(sentence[verb])) continue;
            for (size_t into = verb + 1; into < sentence.size() && into <= verb + kObjectTokens; ++into) {
                if (IsVerb(sentence[into])) break;
                if (!IsInto(sentence[into].text)) continue;
                const Place place = PlaceAfter(sentence, into);
                if (!place.found) continue;
                Instruction(sentence, verb, into, place);
                break;
            }
        }
    }

private:
    std::vector<std::string> Named(const std::vector<std::string>& list, const std::string& name) const {
        std::vector<std::string> found;
        for (const std::string& path : list)
            if (SameText(LastSegment(path), name)) found.push_back(path);
        return found;
    }

    void Add(const Sentence& sentence, ReadmeTarget target, const std::string& file, const std::string& folder, bool level_files) {
        ReadmeRule rule;
        rule.target = target;
        rule.file = file;
        rule.folder = folder;
        rule.level_files = level_files;
        rule.optional = Optional(sentence);
        rule.sentence = Quote(sentence);
        notes_.rules.push_back(std::move(rule));
    }

    static bool Optional(const Sentence& sentence) { return Has(sentence, "optional") || Has(sentence, "if"); }

    void Ambiguous(const Sentence& sentence) {
        const std::string quote = Quote(sentence);
        if (std::find(notes_.ambiguous.begin(), notes_.ambiguous.end(), quote) == notes_.ambiguous.end()) notes_.ambiguous.push_back(quote);
    }

    void Instruction(const Sentence& sentence, size_t verb, size_t into, const Place& place) {
        bool level_files = false;
        for (size_t i = verb + 1; i < into; ++i) {
            const std::string& text = sentence[i].text;
            const std::string name = LastSegment(text);
            if (text == "files" || text == "everything") level_files = true;
            if (LooksLikeFile(text)) {
                File(sentence, name, place);
                continue;
            }
            const std::vector<std::string> folders = Named(folders_, name);
            if (folders.empty() || !FolderMeant(sentence, i, into)) continue;
            Folder(sentence, folders, place);
        }
        if (level_files && place.known && place.target == ReadmeTarget::kData) Add(sentence, ReadmeTarget::kData, "", "", true);
    }

    // "the anim and texhw folders", "the 'MyMod' folder".
    static bool FolderMeant(const Sentence& sentence, size_t at, size_t into) {
        for (size_t i = at + 1; i < into && i <= at + 3; ++i)
            if (IsFolderWord(sentence[i].text)) return true;
        return false;
    }

    void File(const Sentence& sentence, const std::string& name, const Place& place) {
        const std::vector<std::string> files = Named(paths_, name);
        if (files.empty()) {
            if (place.known && std::find(notes_.absent.begin(), notes_.absent.end(), name) == notes_.absent.end()) notes_.absent.push_back(name);
            return;
        }
        if (LowerAscii(name.substr(name.rfind('.') + 1)) == "ini") return;
        if (files.size() > 1 || !place.known || !FitsTarget(place.target, name)) return Optional(sentence) ? void() : Ambiguous(sentence);
        Add(sentence, place.target, files[0], "", false);
    }

    void Folder(const Sentence& sentence, std::vector<std::string> folders, const Place& place) {
        if (folders.size() > 1 && place.known && place.target == ReadmeTarget::kModRoot)
            std::erase_if(folders, [this](const std::string& f) { return std::find(paths_.begin(), paths_.end(), f + "/fm.ini") == paths_.end(); });
        ReadmeTarget own = ReadmeTarget::kData;
        if (!place.known && TargetNamed(LowerAscii(LastSegment(folders[0])), own) && own != ReadmeTarget::kModRoot) return; // already laid out as in the game's folder
        if (folders.size() != 1 || !place.known) return Optional(sentence) ? void() : Ambiguous(sentence);
        const bool content = place.target == ReadmeTarget::kData || place.target == ReadmeTarget::kModRoot;
        if (!content) return Ambiguous(sentence);
        Add(sentence, place.target, "", folders[0], false);
    }

    void Configs(const Sentence& sentence) {
        for (const Token& token : sentence) {
            const std::string name = LastSegment(token.text);
            if (name.rfind("trespasser.", 0) != 0 || name == "trespasser.ini" || name.size() < 5 || name.substr(name.size() - 4) != ".ini") continue;
            const std::vector<std::string> files = Named(paths_, name);
            if (files.size() != 1 || std::find(notes_.configs.begin(), notes_.configs.end(), files[0]) != notes_.configs.end()) continue;
            notes_.configs.push_back(files[0]);
        }
    }

    static void Raise(Need& need, std::string& evidence, Need found, const Sentence& sentence) {
        if (found <= need) return;
        need = found;
        evidence = Quote(sentence);
    }

    static bool MentionsCe(const Sentence& sentence) {
        static constexpr const char* kAfter[] = {"package", "packages", "patch", "version", "directory", "folder", "install", "installation"};
        for (size_t i = 0; i < sentence.size(); ++i) {
            if (sentence[i].text == "tresce" || sentence[i].text == "trespasserce") return true;
            if (sentence[i].raw != "CE") continue;
            if (i > 0 && (sentence[i - 1].text == "trespasser" || sentence[i - 1].text == "tres" || sentence[i - 1].text == "recommended")) return true;
            if (i + 1 < sentence.size() && OneOf(sentence[i + 1].text, kAfter)) return true;
        }
        return false;
    }

    void Dependencies(const Sentence& sentence) {
        const bool negated = Has(sentence, "not") || Has(sentence, "isn't") || Has(sentence, "without");
        const bool needs = Has(sentence, "required") || Has(sentence, "requires") || Has(sentence, "require") || Has(sentence, "needed") ||
                           Has(sentence, "needs") || Has(sentence, "must");
        const bool optional = Has(sentence, "optional") || Has(sentence, "if") || Has(sentence, "optimized");
        const bool recommended = Has(sentence, "recommended") || Has(sentence, "recommend") || Has(sentence, "tested");
        if (MentionsCe(sentence)) {
            const bool only = Has(sentence, "only") && (Has(sentence, "run") || Has(sentence, "played") || Has(sentence, "works"));
            const Need found = !negated && (needs || only) ? Need::kRequired : recommended ? Need::kRecommended : optional || (needs && negated) ? Need::kOptional : Need::kMentioned;
            Raise(notes_.ce, notes_.ce_evidence, found, sentence);
        }
        if (Has(sentence, "atx")) {
            const Need found = needs && !negated ? Need::kRequired : recommended ? Need::kRecommended : needs || optional ? Need::kOptional : Need::kMentioned;
            Raise(notes_.atx, notes_.atx_evidence, found, sentence);
        }
        const bool cannot = Has(sentence, "cannot") || Has(sentence, "can't") || Has(sentence, "not") || Has(sentence, "doesn't");
        if (cannot && (Has(sentence, "restarted") || Has(sentence, "restart"))) notes_.no_restart = true;
    }

    const std::vector<std::string>& paths_;
    const std::vector<std::string>& folders_;
    ReadmeNotes& notes_;
};

} // namespace

const char* ReadmeTargetFolder(ReadmeTarget target) {
    switch (target) {
    case ReadmeTarget::kData: return "data";
    case ReadmeTarget::kAnim: return "data/anim";
    case ReadmeTarget::kMenu: return "data/menu";
    case ReadmeTarget::kTexhw: return "data/texhw";
    case ReadmeTarget::kMaps: return "maps";
    case ReadmeTarget::kModRoot: break;
    }
    return "";
}

const char* NeedName(Need need) {
    switch (need) {
    case Need::kMentioned: return "mentioned";
    case Need::kOptional: return "optional";
    case Need::kRecommended: return "recommended";
    case Need::kRequired: return "required";
    case Need::kNone: break;
    }
    return "none";
}

ReadmeNotes ReadReadme(const std::string& text, const std::vector<std::string>& paths, const std::vector<std::string>& folders) {
    ReadmeNotes notes;
    Reader reader(paths, folders, notes);
    for (const Sentence& sentence : Sentences(text)) reader.Read(sentence);
    return notes;
}

} // namespace jpt
