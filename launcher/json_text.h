// JSON written in order, one item a line.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace jpt {

class JsonText {
public:
    JsonText& Open(const char* key, char bracket) {
        Key(key);
        out_ += bracket;
        closers_.push_back(bracket == '{' ? '}' : ']');
        first_ = true;
        return *this;
    }
    JsonText& Close() {
        const char closer = closers_.back();
        closers_.pop_back();
        if (!first_) Line();
        out_ += closer;
        first_ = false;
        return *this;
    }
    JsonText& Text(const char* key, const std::string& value) {
        Key(key);
        Quote(value);
        return *this;
    }
    JsonText& Flag(const char* key, bool value) {
        Key(key);
        out_ += value ? "true" : "false";
        return *this;
    }
    JsonText& Number(const char* key, long value) {
        Key(key);
        out_ += std::to_string(value);
        return *this;
    }
    JsonText& List(const char* key, const std::vector<std::string>& values) {
        Open(key, '[');
        for (const std::string& value : values) Text(nullptr, value);
        return Close();
    }
    const std::string& str() const { return out_; }

private:
    void Line() {
        out_ += '\n';
        out_.append(closers_.size(), ' ');
    }
    void Key(const char* key) {
        if (!closers_.empty()) {
            if (!first_) out_ += ',';
            Line();
        }
        first_ = false;
        if (!key) return;
        Quote(key);
        out_ += ": ";
    }
    void Quote(const std::string& text) {
        out_ += '"';
        for (const char c : text) {
            const unsigned char byte = static_cast<unsigned char>(c);
            if (c == '"' || c == '\\') {
                out_ += '\\';
                out_ += c;
            } else if (byte < 0x20) {
                char code[8];
                std::snprintf(code, sizeof code, "\\u%04x", byte);
                out_ += code;
            } else {
                out_ += c;
            }
        }
        out_ += '"';
    }

    std::string out_, closers_;
    bool first_ = true;
};

} // namespace jpt
