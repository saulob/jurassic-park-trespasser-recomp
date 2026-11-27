// The installation notes of a level package, read by a fixed set of sentence shapes over the package's own file names. What no
// shape matches is left alone; nothing in a text is ever run, and a destination is only ever one of the folders below.
#pragma once

#include <string>
#include <vector>

namespace jpt {

enum class ReadmeTarget { kData, kAnim, kMenu, kTexhw, kMaps, kModRoot };

const char* ReadmeTargetFolder(ReadmeTarget target); // "data/anim", "maps"; empty for kModRoot

struct ReadmeRule {
    ReadmeTarget target = ReadmeTarget::kData;
    std::string file;   // a path of the package
    std::string folder; // a folder of the package ("anim", "Mods/MyMod")
    bool level_files = false; // "the level files", without names
    bool optional = false;
    std::string sentence; // where the instruction is: "<source>, sentence <n>"
};

enum class Need { kNone, kMentioned, kOptional, kRecommended, kRequired };

const char* NeedName(Need need);

struct ReadmeNotes {
    std::vector<ReadmeRule> rules;
    std::vector<std::string> configs;   // paths of the package a text installs as the game's own settings file
    std::vector<std::string> ambiguous; // instructions about the package's files that are not followed
    std::vector<std::string> absent;    // files an instruction names and the package lacks
    Need ce = Need::kNone, atx = Need::kNone;
    std::string ce_evidence, atx_evidence;
    bool no_restart = false;
};

// paths: every file of the package; folders: every folder of it. Both with '/'. source: the text's path, cited in place of its words.
ReadmeNotes ReadReadme(const std::string& text, const std::vector<std::string>& paths, const std::vector<std::string>& folders, const std::string& source = {});

} // namespace jpt
