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

                if (boost::starts_with(codepoint_str, "100000")) {
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

bool sutils::starts_with(const std::string &source, const std::string &prefix, const size_t begin = 0) noexcept {
    if (source.size() < begin + prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++){
        if (source[i + begin] != prefix[i]) return false;
    }
    return true;
}

struct escape_code {
    char from[6];
    char to;
} static constexpr ESCAPES[4]{
    {"&#44;", ','}, 
    {"&#91;", '['}, 
    {"&#93;", ']'}, 
    {"&amp;", '&'}
};

std::string sutils::cq_escape(const std::string &source, const bool escape_comma) noexcept {
    std::vector<char> buff;
    size_t start = !escape_comma;
    for (size_t i = 0; i < source.size(); i++) {
        bool escaped = false;
        for (size_t j = start; j < 4; j++) {
            if (source[i] == ESCAPES[j].to) {
                size_t k = 0;
                while (ESCAPES[j].from[k])
                    buff.push_back(ESCAPES[j].from[k++]);
                escaped = true;
                break;
            }
        }
        if (!escaped)
            buff.push_back(source[i]);
    }
    buff.push_back('\0');
    return std::string(buff.data());
}

std::string sutils::cq_unescape(const std::string &source) noexcept {
    std::vector<char> buff;
    size_t i = 0;
    while (true) {
        while (source[i] != '&') {
            buff.push_back(source[i]);
            if (!source[i])
                break;
            i++;
        }

        if (i + 5 > source.size()) {
            while (source[i]) {
                buff.push_back(source[i]);
                i++;
            }
            buff.push_back('\0');
            return std::string(buff.data());
        }

        for (size_t j = 0; j < 4; j++) {
            size_t k = 0;
            while (ESCAPES[j].from[k] && source[i + k] == ESCAPES[j].from[k])
                k++;

            if (k == 5) {
                buff.push_back(ESCAPES[j].to);
                i += 5;
                break;
            }
        }
    }
    buff.push_back('\0');
    return std::string(buff.data());
}

void sutils::split_string_by_char(std::vector<std::string> &container, std::string source, char splitter) noexcept {
    size_t i = 0;
    while (source[i]) {
        std::vector<char> buff;
        while (source[i] && source[i] != splitter)
            buff.push_back(source[i++]);
        if (buff.size() > 0) {
            buff.push_back('\0');
            container.emplace_back(buff.data());
        }
    }
}

static constexpr char cq_prefix [] ="[CQ:";
static constexpr size_t cq_prefix_len = sizeof(cq_prefix)-1;
static bool c_starts_with(const char * source, size_t source_len, const char * prefix, size_t prefix_len, const size_t begin = 0) noexcept {
    if (source_len < begin + prefix_len) return false;
    for (size_t i = 0; i < prefix_len; i++){
        if (source[i + begin] != prefix[i]) return false;
    }
    return true;
}

void sutils::cq_disasemble(const std::string & source, std::vector<cq_disasemblies> & container) noexcept {

	std::size_t operation_pos = 0;
	std::size_t panic_pos = 0;
	std::size_t next = 0;

	auto panic_end = [&]() {
		sutils::cq_disasemblies item = {
			"",
			{params_pair("", source.substr(panic_pos))}
		};
		container.emplace_back(std::move(item));
	};

	for (; operation_pos < source.size();) {
		std::size_t pos_of_lsbracket = source.find_first_of('[', operation_pos);
		if (pos_of_lsbracket == std::string::npos) {
			panic_end();
			return;
		}

		next = cq_prefix_len;
		if (!c_starts_with(source.data(), source.size(), cq_prefix, cq_prefix_len, pos_of_lsbracket)) {
			operation_pos += next;
			continue;
		}
			
		int mode = 0;

		std::size_t i = pos_of_lsbracket + cq_prefix_len;
		std::size_t pos_of_rsbracket = std::string::npos;

		for (; i < source.size(); i++) {
			if((source[i] >= 'A' && source[i] <= 'Z')
				|| (source[i] >= 'a' && source[i] <= 'z')
				|| (source[i] >= '0' && source[i] <= '9')) continue;
			// "[CQ:where,"
			else if(source[i] == ',') {
				pos_of_rsbracket = source.find_first_of(']', i + 1);
				if (pos_of_rsbracket == std::string::npos) {
					panic_end();
					return;
				}
				else {
					mode = 1;
				}
				break;
			}
			// "[CQ:what]"
			else if(source[i] == ']') {
				pos_of_rsbracket = i;
				mode = 2;
				break;
			}
			else break;
		}

		if (mode > 0) {
			if (pos_of_lsbracket > panic_pos) {
				sutils::cq_disasemblies preifx = {
					"",
					{{"",source.substr(panic_pos, pos_of_lsbracket - panic_pos)}}
				};
				container.emplace_back(std::move(preifx));
			}

			if (mode == 1) {

				sutils::cq_disasemblies item = {
					source.substr(pos_of_lsbracket + cq_prefix_len, i - pos_of_lsbracket - cq_prefix_len),
					{}
				};
				std::size_t after_comma = i + 1;
				
				auto push_pair = [&](std::string && par1, std::string && par2){
					params_pair ppair = { std::move(par1), std::move(par2) };
					item.params.emplace_back(std::move(ppair));
				};

                auto trim_params = [&](std::size_t begin, std::size_t end)->std::string{
                    // trim, possibly useless
					while (source[begin] == ' ') begin++;
					while (source[end] == ' ') end--;
                    return source.substr(begin, end + 1 - begin);
                };

				for (;;) {

                    // "[CQ:what,params=123123,"
                    //                 ^
					std::size_t pos_of_equal = source.find_first_of('=', after_comma);

                    // "[CQ:what,params=123123,"
                    //           ^----^
					std::string params1 = trim_params(after_comma, pos_of_equal - 1);

                    // "[CQ:what,params=123123,"
                    //                        ^
					std::size_t pos_of_comma = source.find_first_of(',', after_comma);
					if (pos_of_comma == std::string::npos || pos_of_comma > pos_of_rsbracket) {

                        // "[CQ:what,params=123123]"
                        //                  ^----^
						std::string params2 = source.substr(pos_of_equal + 1, pos_of_rsbracket - pos_of_equal - 1);
						push_pair(std::move(params1), std::move(params2));
						break;
					}
					else {
                        // "[CQ:what,params=123123,"
                        //                  ^----^
						std::string params2 = source.substr(pos_of_equal + 1, pos_of_comma - pos_of_equal - 1);
						push_pair(std::move(params1), std::move(params2));

                        // "[CQ:what,params=123123,params=1234"
                        //                        ^
						after_comma = pos_of_comma + 1;
						continue;
					}
				}
				container.emplace_back(std::move(item));
				panic_pos = operation_pos = pos_of_rsbracket + 1;
			}
			else if (mode == 2) {
				sutils::cq_disasemblies item = {
					source.substr(pos_of_lsbracket + cq_prefix_len, i - pos_of_lsbracket - cq_prefix_len),
					{}
				};
				container.emplace_back(std::move(item));
				panic_pos = operation_pos = i + 1;
			}

		}
		else operation_pos = i + 1;
	}
}