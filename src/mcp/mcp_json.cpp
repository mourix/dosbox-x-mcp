/*
 *  DOSBox-X MCP — minimal JSON value, parser and serializer (impl).
 *  See mcp_json.h. Compiled only under --enable-mcp.
 */

#include "mcp_json.h"

#if C_MCP

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mcp {

// -- constructors ----------------------------------------------------------

Json Json::boolean(bool v)            { Json j; j.t_ = Bool;   j.bool_ = v; return j; }
Json Json::number(double v)           { Json j; j.t_ = Number; j.num_ = v; j.isInt_ = false; return j; }
Json Json::str(const std::string &v)  { Json j; j.t_ = String; j.str_ = v; return j; }
Json Json::array()                    { Json j; j.t_ = Array;  return j; }
Json Json::object()                   { Json j; j.t_ = Object; return j; }

Json Json::integer(long long v) {
    Json j; j.t_ = Number; j.isInt_ = true; j.int_ = v; j.num_ = (double)v; return j;
}

// -- accessors -------------------------------------------------------------

double Json::asNumber(double def) const {
    if (t_ != Number) return def;
    return isInt_ ? (double)int_ : num_;
}

long long Json::asInt(long long def) const {
    if (t_ != Number) return def;
    return isInt_ ? int_ : (long long)num_;
}

void Json::set(const std::string &key, const Json &v) {
    if (t_ != Object) { t_ = Object; arr_.clear(); }
    for (auto &kv : obj_) {
        if (kv.first == key) { kv.second = v; return; }
    }
    obj_.push_back(std::make_pair(key, v));
}

const Json *Json::find(const std::string &key) const {
    if (t_ != Object) return nullptr;
    for (auto &kv : obj_) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

void Json::push(const Json &v) {
    if (t_ != Array) { t_ = Array; obj_.clear(); }
    arr_.push_back(v);
}

const Json &Json::at(size_t i) const {
    static const Json kNull;
    if (t_ != Array || i >= arr_.size()) return kNull;
    return arr_[i];
}

// -- serialize -------------------------------------------------------------

void Json::encodeString(const std::string &s, std::string &o) {
    o.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    o += buf;
                } else {
                    o.push_back((char)c);
                }
        }
    }
    o.push_back('"');
}

void Json::serializeTo(std::string &o) const {
    switch (t_) {
        case Null:   o += "null"; break;
        case Bool:   o += (bool_ ? "true" : "false"); break;
        case String: encodeString(str_, o); break;
        case Number: {
            char buf[32];
            if (isInt_) {
                std::snprintf(buf, sizeof(buf), "%lld", int_);
            } else if (!std::isfinite(num_)) {
                o += "null"; break;          /* JSON has no NaN/Inf */
            } else if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%.0f", num_);
            } else {
                std::snprintf(buf, sizeof(buf), "%.17g", num_);
            }
            o += buf;
            break;
        }
        case Array: {
            o.push_back('[');
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) o.push_back(',');
                arr_[i].serializeTo(o);
            }
            o.push_back(']');
            break;
        }
        case Object: {
            o.push_back('{');
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) o.push_back(',');
                encodeString(obj_[i].first, o);
                o.push_back(':');
                obj_[i].second.serializeTo(o);
            }
            o.push_back('}');
            break;
        }
    }
}

std::string Json::serialize() const {
    std::string o;
    serializeTo(o);
    return o;
}

// -- parse -----------------------------------------------------------------

namespace {

/* Cap nesting so a hostile/garbled client cannot blow the stack with deeply
 * nested arrays/objects (a 64 KiB line of '[' would otherwise recurse ~64 K
 * deep). JSON-RPC control traffic is shallow; 64 is far more than real requests
 * need. Hit means the document is rejected as a parse error. */
const int kMaxDepth = 64;

struct Parser {
    const char *p;
    const char *end;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parseValue(Json &out, int depth);

    bool parseString(std::string &s) {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= end) return false;
                char e = *p++;
                switch (e) {
                    case '"':  s.push_back('"');  break;
                    case '\\': s.push_back('\\'); break;
                    case '/':  s.push_back('/');  break;
                    case 'b':  s.push_back('\b'); break;
                    case 'f':  s.push_back('\f'); break;
                    case 'n':  s.push_back('\n'); break;
                    case 'r':  s.push_back('\r'); break;
                    case 't':  s.push_back('\t'); break;
                    case 'u': {
                        if (end - p < 4) return false;
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return false;
                        }
                        /* Encode the BMP code point as UTF-8. Surrogate pairs are
                         * not stitched (sufficient for JSON-RPC control traffic). */
                        if (cp < 0x80) {
                            s.push_back((char)cp);
                        } else if (cp < 0x800) {
                            s.push_back((char)(0xC0 | (cp >> 6)));
                            s.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            s.push_back((char)(0xE0 | (cp >> 12)));
                            s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            s.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: return false;
                }
            } else {
                s.push_back(c);
            }
        }
        return false; /* unterminated */
    }

    bool parseNumber(Json &out) {
        const char *start = p;
        bool isInt = true;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && *p >= '0' && *p <= '9') ++p;
        if (p < end && *p == '.') { isInt = false; ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
        if (p < end && (*p == 'e' || *p == 'E')) {
            isInt = false; ++p;
            if (p < end && (*p == '-' || *p == '+')) ++p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p == start) return false;
        std::string tok(start, (size_t)(p - start));
        if (isInt) {
            errno = 0;
            char *e = nullptr;
            long long v = std::strtoll(tok.c_str(), &e, 10);
            if (e && *e == '\0' && errno == 0) { out = Json::integer(v); return true; }
        }
        out = Json::number(std::strtod(tok.c_str(), nullptr));
        return true;
    }

    bool literal(const char *lit, const Json &v, Json &out) {
        size_t n = 0; while (lit[n]) ++n;
        if ((size_t)(end - p) < n) return false;
        for (size_t i = 0; i < n; ++i) if (p[i] != lit[i]) return false;
        p += n; out = v; return true;
    }
};

bool Parser::parseValue(Json &out, int depth) {
    skipWs();
    if (p >= end) return false;
    char c = *p;
    switch (c) {
        case '{': {
            if (depth >= kMaxDepth) return false;   /* too deeply nested */
            ++p; out = Json::object(); skipWs();
            if (p < end && *p == '}') { ++p; return true; }
            for (;;) {
                skipWs();
                std::string key;
                if (!parseString(key)) return false;
                skipWs();
                if (p >= end || *p != ':') return false;
                ++p;
                Json v;
                if (!parseValue(v, depth + 1)) return false;
                out.set(key, v);
                skipWs();
                if (p >= end) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return false;
            }
        }
        case '[': {
            if (depth >= kMaxDepth) return false;   /* too deeply nested */
            ++p; out = Json::array(); skipWs();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Json v;
                if (!parseValue(v, depth + 1)) return false;
                out.push(v);
                skipWs();
                if (p >= end) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return false;
            }
        }
        case '"': {
            std::string s;
            if (!parseString(s)) return false;
            out = Json::str(s);
            return true;
        }
        case 't': return literal("true",  Json::boolean(true),  out);
        case 'f': return literal("false", Json::boolean(false), out);
        case 'n': return literal("null",  Json::null(),         out);
        default:
            return parseNumber(out);
    }
}

} // namespace

bool Json::parse(const std::string &in, Json &out) {
    Parser ps;
    ps.p = in.c_str();
    ps.end = ps.p + in.size();
    Json v;
    if (!ps.parseValue(v, 0)) return false;
    ps.skipWs();
    if (ps.p != ps.end) return false; /* trailing garbage */
    out = v;
    return true;
}

} // namespace mcp

#endif /* C_MCP */
