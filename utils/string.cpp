#include "./string.h"

#include <iconv.h>
#include <codecvt>

#include "../app.h"
#include "./memory.h"

using namespace std;

namespace cq::utils {
    string sregex_replace(const string &str, const regex &re, const function<string(const smatch &)> fmt_func) {
        string result;
        auto last_end_pos = 0;
        for (sregex_iterator it(str.begin(), str.end(), re), end; it != end; ++it) {
            result += it->prefix().str() + fmt_func(*it);
            last_end_pos = it->position() + it->length();
        }
        result += str.substr(last_end_pos);
        return result;
    }

    static shared_ptr<wchar_t> multibyte_to_widechar(const unsigned code_page, const char *multibyte_str) {
        const auto len = MultiByteToWideChar(code_page, 0, multibyte_str, -1, nullptr, 0);
        auto c_wstr_sptr = make_shared_array<wchar_t>(len + 1);
        MultiByteToWideChar(code_page, 0, multibyte_str, -1, c_wstr_sptr.get(), len);
        return c_wstr_sptr;
    }

    static shared_ptr<char> widechar_to_multibyte(const unsigned code_page, const wchar_t *widechar_str) {
        const auto len = WideCharToMultiByte(code_page, 0, widechar_str, -1, nullptr, 0, nullptr, nullptr);
        auto c_str_sptr = make_shared_array<char>(len + 1);
        WideCharToMultiByte(code_page, 0, widechar_str, -1, c_str_sptr.get(), len, nullptr, nullptr);
        return c_str_sptr;
    }

    string string_encode(const string &s, const Encoding encoding) {
        return widechar_to_multibyte(static_cast<unsigned>(encoding), s2ws(s).c_str()).get();
    }

    string string_decode(const string &b, const Encoding encoding) {
        return ws2s(wstring(multibyte_to_widechar(static_cast<unsigned>(encoding), b.c_str()).get()));
    }

    string string_convert_encoding(const string &text, const string &from_enc, const string &to_enc,
                                   const float capability_factor) {
        string result;

        const auto cd = iconv_open(to_enc.c_str(), from_enc.c_str());
        auto in = const_cast<char *>(text.data());
        auto in_bytes_left = text.size();

        if (in_bytes_left == 0) {
            return result;
        }

        auto out_bytes_left =
            static_cast<decltype(in_bytes_left)>(static_cast<double>(in_bytes_left) * capability_factor);
        auto out = new char[out_bytes_left]{0};
        const auto out_begin = out;

        try {
            if (static_cast<size_t>(-1) != iconv(cd, &in, &in_bytes_left, &out, &out_bytes_left)) {
                // successfully converted
                result = out_begin;
            }
        } catch (...) {
        }

        delete[] out_begin;
        iconv_close(cd);

        return result;
    }

    string string_encode(const string &s, const string &encoding, const float capability_factor) {
        return string_convert_encoding(s, "utf-8", encoding, capability_factor);
    }

    string string_decode(const string &b, const string &encoding, const float capability_factor) {
        return string_convert_encoding(b, encoding, "utf-8", capability_factor);
    }

    string string_to_coolq(const string &str) {
        // call CoolQ API
        return string_encode(str, "gb18030");
    }

    string string_from_coolq(const string &str) {
        // handle CoolQ event or data
        auto result = string_decode(str, "gb18030");

        if (config.convert_unicode_emoji) {
            result = sregex_replace(result, regex(R"(\[CQ:emoji,\s*id=(\d+)\])"), [](const smatch &m) {
                const auto codepoint_str = m.str(1);
                u32string u32_str;

                if (sutils::starts_with(codepoint_str, "100000")) {
                    // keycap # to keycap 9
                    const auto codepoint = static_cast<char32_t>(stoul(codepoint_str.substr(strlen("100000"))));
                    u32_str.append({codepoint, 0xFE0F, 0x20E3});
                } else {
                    const auto codepoint = static_cast<char32_t>(stoul(codepoint_str));
                    u32_str.append({codepoint});
                }

                const auto p = reinterpret_cast<const uint32_t *>(u32_str.data());
                wstring_convert<codecvt_utf8<uint32_t>, uint32_t> conv;
                return conv.to_bytes(p, p + u32_str.size());
            });

            // CoolQ sometimes use "#\uFE0F" to represent "#\uFE0F\u20E3"
            // we should convert them into correct emoji codepoints here
            //     \uFE0F == \xef\xb8\x8f
            //     \u20E3 == \xe2\x83\xa3
            result = sregex_replace(result, regex("([#*0-9]\xef\xb8\x8f)(\xe2\x83\xa3)?"), [](const smatch &m) {
                return m.str(1) + "\xe2\x83\xa3";
            });
        }

        return result;
    }

    string ws2s(const wstring &ws) { return wstring_convert<codecvt_utf8<wchar_t>, wchar_t>().to_bytes(ws); }

    wstring s2ws(const string &s) { return wstring_convert<codecvt_utf8<wchar_t>, wchar_t>().from_bytes(s); }

    string ansi(const string &s) { return string_encode(s, Encoding::ANSI); }
} // namespace cq::utils

bool sutils::starts_with(const std::string &source, const std::string &prefix, const size_t begin) noexcept {
    if (source.size() < begin + prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (source[i + begin] != prefix[i]) return false;
    }
    return true;
}

struct escape_code {
    char from[6];
    char to;
} static constexpr ESCAPES[4]{{"&#44;", ','}, {"&#91;", '['}, {"&#93;", ']'}, {"&amp;", '&'}};

static constexpr std::size_t ESCAPE_LEN = sizeof(ESCAPES[0].from) - 1;
static constexpr std::size_t ESCAPE_CT = sizeof(ESCAPES) / sizeof(ESCAPES[0]);

static inline size_t escape_char_hash(char c, const bool escape_comma) {
    switch (c) {
    case ',':
        if (escape_comma)
            return 0;
        else
            return std::string::npos;
    case '[':
        return 1;
    case ']':
        return 2;
    case '&':
        return 3;
    default:
        return std::string::npos;
    }
}

static const std::size_t buff_alloc_size = 32;

std::string sutils::cq_escape(const std::string &source, const bool escape_comma) noexcept {
    std::vector<char> buff(source.size() + 1);
    std::size_t cursor = 0;

    auto insert = [&](char c) {
        if (cursor >= buff.size()) buff.resize(buff.size() + buff_alloc_size);
        buff[cursor] = c;
        cursor++;
    };

    for (std::size_t i = 0; i < source.size(); i++) {
        std::size_t which = escape_char_hash(source[i], escape_comma);
        if (which != std::string::npos) {
            std::size_t k = 0;
            while (ESCAPES[which].from[k]) insert(ESCAPES[which].from[k++]);
            continue;
        } else
            insert(source[i]);
    }
    insert('\0');
    return std::string(buff.data());
}

std::string sutils::cq_unescape(const std::string &source) noexcept {
    std::vector<char> buff(source.size() + 1);
    std::size_t cursor = 0;

    auto insert = [&](char c) { buff[cursor++] = c; };

    size_t i = 0;
    while (true) {
        while (source[i] != '&') {
            insert(source[i]);
            if (!source[i]) return std::string(buff.data());
            i++;
        }

        if (i + ESCAPE_LEN > source.size()) {
            insert(source[i++]);
            continue;
        }

        std::size_t j = 0, k = 0;
        for (; j < ESCAPE_CT; j++) {
            while (ESCAPES[j].from[k] && source[i + k] == ESCAPES[j].from[k]) k++;
            if (k == 5) break;
        }
        if (k == ESCAPE_LEN) {
            insert(ESCAPES[j].to);
            i += ESCAPE_LEN;
        } else
            insert(source[i++]);
    }
    return std::string(buff.data());
}

void sutils::split_string_by_char(std::vector<std::string> &container, std::string source, char splitter) noexcept {
    size_t i = 0;
    while (source[i]) {
        std::vector<char> buff;
        while (source[i] && source[i] != splitter) buff.push_back(source[i++]);
        if (buff.size() > 0) {
            buff.push_back('\0');
            container.emplace_back(buff.data());
        }
    }
}

static constexpr char cq_prefix[] = "[CQ:";
static constexpr size_t cq_prefix_len = sizeof(cq_prefix) - 1;

enum class CQ_BLOCK_DETECT_TYPE {
    none, // set mode to none when ill-formed
    cq_type, // "[CQ:t", type is always longer than 0
    cq_type_follow, // "[CQ:type"
    cq_para_first, // "[CQ:type, f", param is always longer than 0, also ignore spaces
    cq_para_first_follow, // "[CQ:type, first"
    cq_para_first_tail, // "[CQ:type, first ", ignore spaces after param
    cq_para_second, // "[CQ:type, first =s",  value is always longer than 0
    cq_para_second_follow, // "[CQ:type, first =second"
    cq_end // "[CQ:type, first =second]"
};

void sutils::cq_disassemble(const std::string &source, std::list<cq_disassemblies> &container) noexcept {
    std::size_t operation_pos = 0; // process will try to find [CQ: from operation position
    std::size_t panic_pos = 0; // panic position is at the beginning of source string, when a well-formed [CQ:] block
                               // ended, panic position will point to the position after ']'

    // there are two situations where panic_end is called
    // 1. well-formed [CQ:] block ended, but there are some text before [CQ:] block, panic_from would point to the place
    // before the beginning '[' of [CQ:] block.
    // 2. full source text ends with no [CQ:] block on tail, panic_from would point to std::string::npos.
    auto panic_end = [&](std::size_t panic_from) {
        std::string content = source.substr(panic_pos, panic_from - panic_pos);
        sutils::cq_disassemblies item = {"text", {{"text", cq_unescape(content)}}};
        container.emplace_back(std::move(item));
    };

    for (; operation_pos < source.size();) {
        // always begin with '['
        std::size_t pos_of_lsbracket = source.find_first_of('[', operation_pos);

        // if '[' cannot be found or remaining text won't fit any [CQ:] block, panic-ends process
        if (pos_of_lsbracket == std::string::npos || source.size() < pos_of_lsbracket + cq_prefix_len) {
            panic_end(std::string::npos);
            return;
        }

        // try to match "[CQ:"
        // if "[CQ:" did not match at operation_pos, jump to #next to find next '[CQ:'
        std::size_t next = pos_of_lsbracket;

        // a assumed start position of "type" in "[CQ:type"
        std::size_t type_begin = pos_of_lsbracket + cq_prefix_len;

        for (; next < type_begin; next++) {
            if (source[next] != cq_prefix[next - pos_of_lsbracket]) break;
        }
        operation_pos = next;
        if (next != type_begin) continue;

        CQ_BLOCK_DETECT_TYPE mode = CQ_BLOCK_DETECT_TYPE::cq_type;

        std::size_t i = type_begin;
        std::size_t pos_of_rsbracket = std::string::npos;

        sutils::cq_disassemblies temp_item;
        std::size_t para_first_begin;
        std::size_t para_first_size;
        std::size_t para_second_begin;

        auto push_pair = [&](std::string &&par1, std::string &&par2) {
            params_pair ppair = {std::move(par1), std::move(par2)};
            temp_item.params.emplace_back(std::move(ppair));
        };

        for (std::size_t i = type_begin; i < source.size(); i++) {
            switch (mode) {
            case CQ_BLOCK_DETECT_TYPE::cq_type: {
                if ((source[i] >= 'A' && source[i] <= 'Z') || (source[i] >= 'a' && source[i] <= 'z')
                    || (source[i] >= '0' && source[i] <= '9')) {
                    mode = CQ_BLOCK_DETECT_TYPE::cq_type_follow;
                } else {
                    operation_pos = i;
                    mode = CQ_BLOCK_DETECT_TYPE::none;
                    break;
                }
                continue;
            }
            case CQ_BLOCK_DETECT_TYPE::cq_type_follow: { // try to get a word behind ':'
                if ((source[i] >= 'A' && source[i] <= 'Z') || (source[i] >= 'a' && source[i] <= 'z')
                    || (source[i] >= '0' && source[i] <= '9')) {
                    continue;
                } else if (source[i] == ',') { // time for parameter
                    // [CQ:what,
                    //     <-->
                    temp_item.type = source.substr(type_begin, i - type_begin);
                    mode = CQ_BLOCK_DETECT_TYPE::cq_para_first;
                } else if (source[i] == ']') { // [CQ:what]
                    temp_item.type = source.substr(type_begin, i - type_begin);
                    mode = CQ_BLOCK_DETECT_TYPE::cq_end;
                    break;
                } else {
                    operation_pos = i;
                    mode = CQ_BLOCK_DETECT_TYPE::none;
                    break;
                }
                continue;
            }
            case CQ_BLOCK_DETECT_TYPE::cq_para_first: { // cq param name may have space on both side
                if (source[i] == ' ')
                    continue;
                else if (source[i] == ']') {
                    mode = CQ_BLOCK_DETECT_TYPE::none;
                    break;
                } else {
                    para_first_begin = i;
                    para_first_size = 1;
                    mode = CQ_BLOCK_DETECT_TYPE::cq_para_first_follow;
                }
                continue;
            }
            case CQ_BLOCK_DETECT_TYPE::cq_para_first_follow: {
                if (source[i] == ' ')
                    mode = CQ_BLOCK_DETECT_TYPE::cq_para_first_tail;
                else if (source[i] == ']') {
                    mode = CQ_BLOCK_DETECT_TYPE::none;
                    break;
                } else if (source[i] == '=')
                    mode = CQ_BLOCK_DETECT_TYPE::cq_para_second;
                else
                    para_first_size++;
                continue;
            }
            case CQ_BLOCK_DETECT_TYPE::cq_para_first_tail: {
                if (source[i] == ' ')
                    continue;
                else if (source[i] == '=')
                    mode = CQ_BLOCK_DETECT_TYPE::cq_para_second;
                else {
                    mode = CQ_BLOCK_DETECT_TYPE::none;
                    break;
                }
                continue;
            }
            case CQ_BLOCK_DETECT_TYPE::cq_para_second: {
                if (source[i] == ']' || source[i] == ',') {
                    mode = CQ_BLOCK_DETECT_TYPE::none;
                    break;
                } else {
                    para_second_begin = i;
                    mode = CQ_BLOCK_DETECT_TYPE::cq_para_second_follow;
                }
                continue;
            }
            case CQ_BLOCK_DETECT_TYPE::cq_para_second_follow: {
                if (source[i] == ']') {
                    push_pair(source.substr(para_first_begin, para_first_size),
                              source.substr(para_second_begin, i - para_second_begin));
                    mode = CQ_BLOCK_DETECT_TYPE::cq_end;
                    break;
                } else if (source[i] == ',') {
                    push_pair(source.substr(para_first_begin, para_first_size),
                              source.substr(para_second_begin, i - para_second_begin));
                    mode = CQ_BLOCK_DETECT_TYPE::cq_para_first;
                    break;
                }
                continue;
            }
            default:
                break;
            }

            if (mode == CQ_BLOCK_DETECT_TYPE::cq_end) {
                if (pos_of_lsbracket > panic_pos) {
                    panic_end(pos_of_lsbracket);
                }
                container.emplace_back(std::move(temp_item));
                operation_pos = panic_pos = i + 1;
                break;
            } else if (mode == CQ_BLOCK_DETECT_TYPE::none) {
                break;
            }
        }
    }
}