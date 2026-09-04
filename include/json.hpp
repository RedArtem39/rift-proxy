#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <algorithm>

namespace json {

enum class Type {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
};

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    Array arr_val;
    Object obj_val;

    Value() : type(Type::Null) {}
    Value(std::nullptr_t) : type(Type::Null) {}
    Value(bool b) : type(Type::Boolean), bool_val(b) {}
    Value(int n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    Value(int64_t n) : type(Type::Number), num_val(static_cast<double>(n)) {}
    Value(double n) : type(Type::Number), num_val(n) {}
    Value(const char* s) : type(Type::String), str_val(s ? s : "") {}
    Value(const std::string& s) : type(Type::String), str_val(s) {}
    Value(const Array& a) : type(Type::Array), arr_val(a) {}
    Value(const Object& o) : type(Type::Object), obj_val(o) {}

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Boolean; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    bool as_bool(bool def = false) const {
        return (type == Type::Boolean) ? bool_val : def;
    }

    int as_int(int def = 0) const {
        return (type == Type::Number) ? static_cast<int>(num_val) : def;
    }

    double as_double(double def = 0.0) const {
        return (type == Type::Number) ? num_val : def;
    }

    std::string as_string(const std::string& def = "") const {
        return (type == Type::String) ? str_val : def;
    }

    const Array& as_array() const {
        static const Array empty_arr;
        return (type == Type::Array) ? arr_val : empty_arr;
    }

    const Object& as_object() const {
        static const Object empty_obj;
        return (type == Type::Object) ? obj_val : empty_obj;
    }

    bool has_field(const std::string& key) const {
        if (type != Type::Object) return false;
        return obj_val.find(key) != obj_val.end();
    }

    const Value& operator[](const std::string& key) const {
        static const Value null_val;
        if (type != Type::Object) return null_val;
        auto it = obj_val.find(key);
        if (it == obj_val.end()) return null_val;
        return it->second;
    }

    Value& operator[](const std::string& key) {
        if (type != Type::Object) {
            type = Type::Object;
            obj_val.clear();
        }
        return obj_val[key];
    }

    const Value& operator[](size_t index) const {
        static const Value null_val;
        if (type != Type::Array || index >= arr_val.size()) return null_val;
        return arr_val[index];
    }
};

class Parser {
    std::string src;
    size_t pos = 0;

    void skip_whitespace() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pos++;
            } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '/') {
                // Line comment support
                pos += 2;
                while (pos < src.size() && src[pos] != '\n') pos++;
            } else if (c == '/' && pos + 1 < src.size() && src[pos + 1] == '*') {
                // Block comment support
                pos += 2;
                while (pos + 1 < src.size() && !(src[pos] == '*' && src[pos + 1] == '/')) pos++;
                if (pos + 1 < src.size()) pos += 2;
            } else {
                break;
            }
        }
    }

    char peek() {
        skip_whitespace();
        if (pos >= src.size()) return 0;
        return src[pos];
    }

    char get() {
        skip_whitespace();
        if (pos >= src.size()) return 0;
        return src[pos++];
    }

    std::string parse_string() {
        if (get() != '"') throw std::runtime_error("Expected '\"'");
        std::string res;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return res;
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                    case '"': res += '"'; break;
                    case '\\': res += '\\'; break;
                    case '/': res += '/'; break;
                    case 'b': res += '\b'; break;
                    case 'f': res += '\f'; break;
                    case 'n': res += '\n'; break;
                    case 'r': res += '\r'; break;
                    case 't': res += '\t'; break;
                    case 'u': {
                        if (pos + 4 <= src.size()) {
                            // Simple hex bypass for basic chars
                            pos += 4;
                            res += '?';
                        }
                        break;
                    }
                    default: res += esc; break;
                }
            } else {
                res += c;
            }
        }
        throw std::runtime_error("Unterminated string");
    }

    Value parse_number() {
        size_t start = pos;
        if (src[pos] == '-') pos++;
        while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' || src[pos] == '+' || src[pos] == '-')) {
            pos++;
        }
        std::string num_str = src.substr(start, pos - start);
        return Value(std::stod(num_str));
    }

    Value parse_array() {
        if (get() != '[') throw std::runtime_error("Expected '['");
        Array arr;
        skip_whitespace();
        if (peek() == ']') {
            get();
            return Value(arr);
        }
        while (true) {
            arr.push_back(parse_value());
            skip_whitespace();
            char next = peek();
            if (next == ']') {
                get();
                break;
            }
            if (next == ',') {
                get();
                continue;
            }
            throw std::runtime_error("Expected ',' or ']' in array");
        }
        return Value(arr);
    }

    Value parse_object() {
        if (get() != '{') throw std::runtime_error("Expected '{'");
        Object obj;
        skip_whitespace();
        if (peek() == '}') {
            get();
            return Value(obj);
        }
        while (true) {
            skip_whitespace();
            if (peek() != '"') throw std::runtime_error("Expected string key in object");
            std::string key = parse_string();
            skip_whitespace();
            if (get() != ':') throw std::runtime_error("Expected ':' after object key");
            Value val = parse_value();
            obj[key] = val;
            skip_whitespace();
            char next = peek();
            if (next == '}') {
                get();
                break;
            }
            if (next == ',') {
                get();
                continue;
            }
            throw std::runtime_error("Expected ',' or '}' in object");
        }
        return Value(obj);
    }

public:
    explicit Parser(std::string text) : src(std::move(text)), pos(0) {}

    Value parse_value() {
        skip_whitespace();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value(parse_string());
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
        if (c == 't' || c == 'f') {
            std::string ident;
            while (pos < src.size() && std::isalpha(static_cast<unsigned char>(src[pos]))) {
                ident += src[pos++];
            }
            if (ident == "true") return Value(true);
            if (ident == "false") return Value(false);
            throw std::runtime_error("Unknown token: " + ident);
        }
        if (c == 'n') {
            std::string ident;
            while (pos < src.size() && std::isalpha(static_cast<unsigned char>(src[pos]))) {
                ident += src[pos++];
            }
            if (ident == "null") return Value(nullptr);
            throw std::runtime_error("Unknown token: " + ident);
        }
        throw std::runtime_error(std::string("Unexpected character: ") + (c ? std::string(1, c) : std::string("EOF")));
    }

    static Value parse(const std::string& str) {
        Parser p(str);
        return p.parse_value();
    }
};

inline std::string serialize(const Value& val, int indent = 0) {
    std::string indent_str(indent * 2, ' ');
    switch (val.type) {
        case Type::Null: return "null";
        case Type::Boolean: return val.bool_val ? "true" : "false";
        case Type::Number: {
            if (val.num_val == static_cast<int64_t>(val.num_val)) {
                return std::to_string(static_cast<int64_t>(val.num_val));
            }
            return std::to_string(val.num_val);
        }
        case Type::String: {
            std::string res = "\"";
            for (char c : val.str_val) {
                if (c == '"') res += "\\\"";
                else if (c == '\\') res += "\\\\";
                else if (c == '\n') res += "\\n";
                else if (c == '\r') res += "\\r";
                else if (c == '\t') res += "\\t";
                else res += c;
            }
            res += "\"";
            return res;
        }
        case Type::Array: {
            if (val.arr_val.empty()) return "[]";
            std::string res = "[\n";
            for (size_t i = 0; i < val.arr_val.size(); ++i) {
                res += indent_str + "  " + serialize(val.arr_val[i], indent + 1);
                if (i + 1 < val.arr_val.size()) res += ",";
                res += "\n";
            }
            res += indent_str + "]";
            return res;
        }
        case Type::Object: {
            if (val.obj_val.empty()) return "{}";
            std::string res = "{\n";
            size_t i = 0;
            for (const auto& [k, v] : val.obj_val) {
                res += indent_str + "  \"" + k + "\": " + serialize(v, indent + 1);
                if (++i < val.obj_val.size()) res += ",";
                res += "\n";
            }
            res += indent_str + "}";
            return res;
        }
    }
    return "null";
}

} // namespace json
