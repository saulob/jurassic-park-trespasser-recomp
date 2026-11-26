// The compression methods of RAR 2.0, 2.9 and 5.0. One of these is kept for a whole archive: solid files go on from its state.
#pragma once

#include <cstdint>
#include <memory>

#include "archive_reader.h"
#include "rar_stream.h"

namespace jpt::rar {

enum class Method { k20, k29, k50, k70 };

class Unpacker {
public:
    Unpacker();
    ~Unpacker();
    // The window, as large as any file of the archive needs; it is made by the first Unpack.
    void SetWindow(size_t size) { window_size_ = size; }
    ArchiveError Unpack(Method method, bool solid, BitSource& in, Output& out);

private:
    class Lz20;
    class Lz29;
    class Lz50;
    template <typename Decoder>
    ArchiveError Run(std::unique_ptr<Decoder>& decoder, bool solid, BitSource& in, Output& out);

    size_t window_size_ = 0;
    Window window_;
    std::unique_ptr<Lz20> lz20_;
    std::unique_ptr<Lz29> lz29_;
    std::unique_ptr<Lz50> lz50_;
};

} // namespace jpt::rar
