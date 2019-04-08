#pragma once

#include "../common.h"

#include <regex>

namespace cq::utils {
    std::string sregex_replace(const std::string &str, const std::regex &re,
                               std::function<std::string(const std::smatch &)> fmt_func);

    enum class Encoding : unsigned {
        // https://msdn.microsoft.com/en-us/library/windows/desktop/dd317756.aspx

        ANSI = 0,
        UTF8 = 65001,
        GB2312 = 936,
        GB18030 = 54936,
    };

    std::string string_encode(const std::string &s, Encoding encoding);
    std::string string_decode(const std::string &b, Encoding encoding);

    std::string string_convert_encoding(const std::string &text, const std::string &from_enc, const std::string &to_enc,
                                        float capability_factor);
    std::string string_encode(const std::string &s, const std::string &encoding, float capability_factor = 2.0f);
    std::string string_decode(const std::string &b, const std::string &encoding, float capability_factor = 2.0f);

    std::string string_to_coolq(const std::string &str);
    std::string string_from_coolq(const std::string &str);

    std::string ws2s(const std::wstring &ws);
    std::wstring s2ws(const std::string &s);
    std::string ansi(const std::string &s);
} // namespace cq::utils

namespace std {
    inline string to_string(const string &val) { return val; }
    inline string to_string(const bool val) { return val ? "true" : "false"; }
} // namespace std

namespace sutils {
    extern bool starts_with(const std::string &s, const std::string &prefix, const size_t begin = 0) noexcept;

    extern std::string cq_escape(const std::string &source, const bool escape_comma) noexcept;
    extern std::string cq_unescape(const std::string &source) noexcept;
    extern void split_string_by_char(std::vector<std::string> &container, std::string source, char splitter) noexcept;

    /* parse "test_text[CQ:what][CQ:where,parama=1234,paramb=123][CQ:why,param=1231234]test_text"
     * result looks like
     *  {
     *      {type: "text",  params: [ {"text", "test_text"} ]},
     *      {type: "what",  params: [ ]},
     *      {type: "where", params: [ {"parama", "1234"}, {"paramb", "123"} ]},
     *      {type: "why",   params: [ {"param", "1231234"} ]},
     *      {type: "text",  params: [ {"text", "test_text"} ]}
     *  }
     * also, "text" result will be cq_unescaped
     */
    using params_pair = std::pair<std::string, std::string>;
    struct cq_disasemblies {
        std::string type;
        std::vector<params_pair> params;
    };

    extern void cq_disasemble(const std::string &source, std::vector<cq_disasemblies> &container) noexcept;
} // namespace sutils