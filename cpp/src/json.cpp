#include "json.h"

#include <cstdio>
#include <cstdlib>

#include "core.h"

namespace {

const Json kNull;

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    bool parse(Json& out) {
        skip();
        if (!value(out)) return false;
        skip();
        return pos_ == s_.size();
    }

private:
    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) ++pos_;
    }

    bool literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& cp) {
        if (pos_ + 4 > s_.size()) return false;
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool value(Json& out) {
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            out = Json::object();
            skip();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            while (true) {
                skip();
                std::string key;
                if (pos_ >= s_.size() || !string(key)) return false;
                skip();
                if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
                skip();
                Json v;
                if (!value(v)) return false;
                out.set(key, std::move(v));
                skip();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (s_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++pos_;
            out = Json::array();
            skip();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            while (true) {
                skip();
                Json v;
                if (!value(v)) return false;
                out.push(std::move(v));
                skip();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (s_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (literal("true")) {
            out = Json(true);
            return true;
        }
        if (literal("false")) {
            out = Json(false);
            return true;
        }
        if (literal("null")) {
            out = Json();
            return true;
        }
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        const double n = std::strtod(begin, &end);
        if (end == begin) return false;
        pos_ += static_cast<size_t>(end - begin);
        out = Json(n);
        return true;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

void escape(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

}  // namespace

Json::Json(const std::wstring& s) : type_(Type::String), str_(toUtf8(s)) {}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json Json::parse(const std::string& text, bool* ok) {
    Json out;
    Parser p(text);
    const bool good = p.parse(out);
    if (ok) *ok = good;
    return good ? out : Json();
}

std::wstring Json::wstring() const { return fromUtf8(str_); }

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

const Json& Json::operator[](size_t index) const { return type_ == Type::Array && index < arr_.size() ? arr_[index] : kNull; }

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
}

bool Json::contains(const std::string& key) const { return type_ == Type::Object && obj_.count(key) > 0; }

Json& Json::push(Json value) {
    if (type_ != Type::Array) *this = array();
    arr_.push_back(std::move(value));
    return *this;
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) *this = object();
    obj_[key] = std::move(value);
    return *this;
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[32];
            if (num_ == static_cast<double>(static_cast<long long>(num_)))
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
            else
                std::snprintf(buf, sizeof(buf), "%.6g", num_);
            out += buf;
            break;
        }
        case Type::String: escape(str_, out); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                arr_[i].dumpTo(out);
            }
            out += ']';
            break;
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : obj_) {
                if (!first) out += ',';
                first = false;
                escape(k, out);
                out += ':';
                v.dumpTo(out);
            }
            out += '}';
            break;
        }
    }
}
