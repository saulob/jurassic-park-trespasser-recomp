// The launcher's own text. Only Português Brasil is translated: every other language, including the ones that translate
// the game, shows the launcher in English. Drawing code keeps calling Tr; a text without a row is shown as it is.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

namespace jpt {

struct UiTextRow {
    const char* english;
    const char* text;
};

extern const UiTextRow kPtBrUiText[];
extern const size_t kPtBrUiTextCount;
// Fragments of messages built at run time (a path or a number between known words).
extern const UiTextRow kPtBrPhrases[];
extern const size_t kPtBrPhraseCount;

// Called every frame with the `language` setting, so a change shows at once.
void SetLauncherLanguage(const std::string& language);
bool LauncherIsPortuguese();

// The result outlives the call.
const char* Tr(const char* english);
std::string Tr(const std::string& english);
// A message with known phrases inside it, each replaced on its own.
std::string TrMessage(const std::string& english);

// The translated format filled in; the arguments keep the English order.
template <typename... Args>
std::string TrFormat(const char* english_format, Args... args) {
    const char* format = Tr(english_format);
    const int size = std::snprintf(nullptr, 0, format, args...);
    std::string text(size > 0 ? size_t(size) : 0, '\0');
    if (size > 0) std::snprintf(text.data(), text.size() + 1, format, args...);
    return text;
}

} // namespace jpt
