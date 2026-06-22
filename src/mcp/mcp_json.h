/*
 *  DOSBox-X MCP — minimal JSON value, parser and serializer.
 *
 *  Stdlib-only, no external dependencies (guardrail: isolation + simplicity).
 *  Just enough JSON to decode newline-delimited JSON-RPC requests and encode
 *  bounded responses. Compiled only under --enable-mcp; see docs/MCP_BUILD_PLAN.md.
 */

#ifndef DOSBOX_MCP_JSON_H
#define DOSBOX_MCP_JSON_H

#include "config.h"

#if C_MCP

#include <string>
#include <vector>
#include <utility>

namespace mcp {

/* A small JSON value. Object key order is preserved (vector of pairs) so encoded
 * responses are deterministic, which keeps the unit-test assertions exact. */
class Json {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Json() : t_(Null), bool_(false), isInt_(false), num_(0.0), int_(0) {}

    static Json null()                       { return Json(); }
    static Json boolean(bool v);
    static Json number(double v);
    static Json integer(long long v);
    static Json str(const std::string &v);
    static Json array();
    static Json object();

    Type type() const { return t_; }
    bool isNull()   const { return t_ == Null; }
    bool isBool()   const { return t_ == Bool; }
    bool isNumber() const { return t_ == Number; }
    bool isString() const { return t_ == String; }
    bool isArray()  const { return t_ == Array; }
    bool isObject() const { return t_ == Object; }

    bool        asBool(bool def = false) const   { return t_ == Bool ? bool_ : def; }
    double      asNumber(double def = 0.0) const;
    long long   asInt(long long def = 0) const;
    const std::string &asString() const          { return str_; }

    /* Object helpers (no-ops / safe defaults on non-objects). */
    void set(const std::string &key, const Json &v);
    const Json *find(const std::string &key) const; /* nullptr if absent */
    bool has(const std::string &key) const { return find(key) != nullptr; }

    /* Array helper. */
    void push(const Json &v);

    /* Encode to a compact JSON string (no whitespace). */
    std::string serialize() const;

    /* Parse a whole JSON document. Returns true and fills out on success; the
     * entire input (minus surrounding whitespace) must be consumed. */
    static bool parse(const std::string &in, Json &out);

private:
    Type t_;
    bool bool_;
    bool isInt_;          /* Number originated as an integer literal/value. */
    double num_;
    long long int_;
    std::string str_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json> > obj_;

    void serializeTo(std::string &o) const;
    static void encodeString(const std::string &s, std::string &o);
};

} // namespace mcp

#endif /* C_MCP */

#endif /* DOSBOX_MCP_JSON_H */
