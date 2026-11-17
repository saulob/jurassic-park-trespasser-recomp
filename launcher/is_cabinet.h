// Read-only InstallShield 5 cabinet that carries its own file table (setup/data1.cab of the Russian disc).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jpt {

struct CabinetFile {
    std::string path;  // '/' separated, as in the cabinet
    uint64_t size = 0;
    uint64_t packed = 0;
    uint64_t offset = 0;
    bool compressed = false;
};

class Cabinet {
public:
    // False with a reason when the bytes are not such a cabinet or its table points outside it.
    bool Open(std::vector<uint8_t> bytes, std::string& error);

    const std::vector<CabinetFile>& files() const { return files_; }
    // Case-insensitive lookup; nullptr when absent.
    const CabinetFile* Find(const std::string& path) const;
    // The file's bytes; false when they do not decode to its recorded size.
    bool Read(const CabinetFile& file, std::vector<uint8_t>& out, std::string& error) const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<CabinetFile> files_;
};

}  // namespace jpt
