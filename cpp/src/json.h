// Небольшой JSON: разбор и сборка строк протокола с Python-«мозгом».
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int n) : type_(Type::Number), num_(n) {}
    Json(double n) : type_(Type::Number), num_(n) {}
    Json(float n) : type_(Type::Number), num_(n) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(const std::wstring& s);

    static Json array();
    static Json object();
    static Json parse(const std::string& text, bool* ok = nullptr);

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    double number(double fallback = 0.0) const { return type_ == Type::Number ? num_ : fallback; }
    int integer(int fallback = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    const std::string& string() const { return str_; }
    std::wstring wstring() const;

    size_t size() const;
    const Json& operator[](size_t index) const;
    const Json& operator[](const std::string& key) const;
    bool contains(const std::string& key) const;
    const std::map<std::string, Json>& items() const { return obj_; }

    Json& push(Json value);
    Json& set(const std::string& key, Json value);

    std::string dump() const;

private:
    void dumpTo(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};
