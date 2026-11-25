// What the Graphics page knows about the graphics cards. The launcher opens without any: a list that could not be made is
// empty and the saved choice stays as it is.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "jpt_gpu.h"

namespace jpt {

struct GraphicsCards {
    enum class State { kNotAsked, kSkipped, kProbed };
    State state = State::kNotAsked;
    std::string detail; // why the list is empty
    std::vector<jpt_gpu_t> gpus;
    std::vector<std::string> names;
    std::vector<unsigned> msaa;
};

struct GraphicsCardSource {
    std::function<std::vector<jpt_gpu_t>(std::string& detail)> list;
    std::function<unsigned(int)> msaa;
};

// The labels of the usable adapters in enumeration order: the Graphics card choices.
inline std::vector<std::string> GpuChoices(const std::vector<jpt_gpu_t>& gpus) {
    std::vector<std::string> choices;
    for (const jpt_gpu_t& gpu : gpus) {
        if (gpu.label[0]) choices.emplace_back(gpu.label);
    }
    return choices;
}

// Asks the system once; `skip` leaves Direct3D and Vulkan alone (software recovery).
inline GraphicsCards DetectGraphicsCards(bool skip, const GraphicsCardSource& source) {
    GraphicsCards cards;
    if (skip) {
        cards.state = GraphicsCards::State::kSkipped;
        return cards;
    }
    cards.state = GraphicsCards::State::kProbed;
    cards.gpus = source.list(cards.detail);
    cards.names = GpuChoices(cards.gpus);
    cards.msaa.assign(cards.gpus.size(), 0);
    for (size_t i = 0; i < cards.gpus.size(); ++i) {
        if (cards.gpus[i].name[0]) cards.msaa[i] = source.msaa(static_cast<int>(i));
    }
    return cards;
}

// The card the game will use for the saved choice (the renderer resolves it the same way); -1 when none is known.
inline int ChosenCard(const GraphicsCards& cards, const std::string& saved) {
    return jpt_gpu_resolve(cards.gpus.data(), static_cast<int>(cards.gpus.size()), saved.c_str());
}

// The label to store in place of `saved`; nothing when it is right already or no card is known.
inline std::optional<std::string> CardToStore(const GraphicsCards& cards, const std::string& saved) {
    const int found = ChosenCard(cards, saved);
    if (found < 0 || saved == cards.gpus[size_t(found)].label) return std::nullopt;
    return std::string(cards.gpus[size_t(found)].label);
}

// The English text of the Graphics card row when there is no card to choose; empty when there is one.
inline const char* GraphicsCardsProblem(const GraphicsCards& cards) {
    if (!cards.names.empty()) return "";
    return cards.state == GraphicsCards::State::kSkipped
               ? "Graphics card detection is off in software recovery mode. The saved choice is kept."
               : "Graphics card unavailable. The saved choice is kept.";
}

inline const char* GraphicsCardsResult(const GraphicsCards& cards) {
    switch (cards.state) {
    case GraphicsCards::State::kNotAsked: return "not_asked";
    case GraphicsCards::State::kSkipped: return "skipped";
    case GraphicsCards::State::kProbed: break;
    }
    return cards.names.empty() ? "unavailable" : "ok";
}

struct SampleLevels {
    std::vector<unsigned> levels; // besides Off
    bool unsupported = false;     // the saved level is not one the card supports
};

// The anti-aliasing levels to offer. Without a known card only the saved one is offered, so that it stays selected.
inline SampleLevels AntiAliasingLevels(const GraphicsCards& cards, const std::string& saved_card, const std::string& saved) {
    SampleLevels result;
    const int gpu = ChosenCard(cards, saved_card);
    bool listed = saved == "0" || saved == "1";
    for (unsigned samples = 2; samples <= 8; samples *= 2) {
        const bool is_saved = saved == std::to_string(samples);
        if (gpu >= 0 ? (cards.msaa[size_t(gpu)] & (1u << samples)) != 0 : is_saved) {
            result.levels.push_back(samples);
            listed = listed || is_saved;
        }
    }
    result.unsupported = !listed;
    return result;
}

} // namespace jpt
