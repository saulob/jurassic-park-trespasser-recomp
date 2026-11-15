#include "localization/ui_text.h"

#include <atomic>
#include <cctype>
#include <string_view>
#include <unordered_map>

namespace jpt {
namespace {

std::atomic<bool> g_portuguese{false};

const std::unordered_map<std::string_view, const char*>& PortugueseIndex() {
    static const auto index = [] {
        std::unordered_map<std::string_view, const char*> map;
        for (size_t i = 0; i < kPtBrUiTextCount; ++i) map.emplace(kPtBrUiText[i].english, kPtBrUiText[i].text);
        return map;
    }();
    return index;
}

bool IsLetter(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }

} // namespace

void SetLauncherLanguage(const std::string& language) { g_portuguese = language == "pt-BR"; }

bool LauncherIsPortuguese() { return g_portuguese; }

const char* Tr(const char* english) {
    if (!english || !g_portuguese) return english;
    const auto& index = PortugueseIndex();
    const auto row = index.find(english);
    return row == index.end() ? english : row->second;
}

std::string Tr(const std::string& english) { return Tr(english.c_str()); }

std::string TrMessage(const std::string& english) {
    if (!g_portuguese) return english;
    std::string out;
    for (size_t i = 0; i < english.size();) {
        const UiTextRow* best = nullptr;
        size_t best_length = 0;
        for (size_t p = 0; p < kPtBrPhraseCount; ++p) {
            const std::string_view phrase = kPtBrPhrases[p].english;
            if (phrase.size() <= best_length || english.compare(i, phrase.size(), phrase) != 0) continue;
            const bool word_start = !IsLetter(phrase.front()) || i == 0 || !IsLetter(english[i - 1]);
            if (word_start) {
                best = &kPtBrPhrases[p];
                best_length = phrase.size();
            }
        }
        if (best) {
            out += best->text;
            i += best_length;
        } else {
            out += english[i++];
        }
    }
    return out;
}

} // namespace jpt
