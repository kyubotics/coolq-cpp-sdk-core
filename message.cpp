#include "./message.h"

#include <sstream>

#include "./api.h"

using namespace std;

namespace cq::message {

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

    std::string cq_escape(const std::string &source, const bool escape_comma) noexcept {
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

    std::string cq_unescape(const std::string &source) noexcept {
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

    using params_pair = std::pair<std::string, std::string>;
    struct cq_disassemblies {
        std::string type;
        std::list<params_pair> params;
    };

    static constexpr char cq_prefix[] = "[CQ:";
    static constexpr size_t cq_prefix_len = sizeof(cq_prefix) - 1;

    enum class CQ_BLOCK_DETECT_TYPE {
        none,
        cq_type,
        cq_type_follow,
        cq_para_first,
        cq_para_first_follow,
        cq_para_first_tail,
        cq_para_second,
        cq_para_second_follow,
        cq_end
    };

    /**
     * parse "test_text[CQ:what][CQ:where,parama=1234,paramb=123][CQ:why,param=1231234]test_text"
     * result looks like
     *  {
     *      {type: "text",  params: [ {"text", "test_text"} ]},
     *      {type: "what",  params: [ ]},
     *      {type: "where", params: [ {"parama", "1234"}, {"paramb", "123"} ]},
     *      {type: "why",   params: [ {"param", "1231234"} ]},
     *      {type: "text",  params: [ {"text", "test_text"} ]}
     *  }
     * also, "text" result will be cq_unescaped
     *
     * it is a local function, so static is introduced here to prevent being linked outside
     */
    static void cq_disassemble(const std::string &source, std::list<cq_disassemblies> &container) noexcept {
        std::size_t operation_pos = 0; // process will try to find [CQ: from operation position
        std::size_t panic_pos = 0; // panic position is at the beginning of source string, when a well-formed [CQ:]
                                   // block ended, panic position will point to the position after ']'

        // there are two situations where panic_end is called
        // 1. well-formed [CQ:] block ended, but there are some text before [CQ:] block, panic_from would point to the
        // place before the beginning '[' of [CQ:] block.
        // 2. full source text ends with no [CQ:] block on tail, panic_from would point to std::string::npos.
        auto panic_end = [&](std::size_t panic_from) {
            std::string content = source.substr(panic_pos, panic_from - panic_pos);
            cq_disassemblies item = {"text", {{"text", cq_unescape(content)}}};
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

            cq_disassemblies temp_item;
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
                    // "[CQ:t", type is always longer than 0
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
                case CQ_BLOCK_DETECT_TYPE::cq_type_follow: {
                    // "[CQ:type"
                    // try to get a word behind ':'
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
                case CQ_BLOCK_DETECT_TYPE::cq_para_first: {
                    // "[CQ:type, f", param is always longer than 0, also ignore spaces
                    // cq param name may have space on both side as original program indicates
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
                    // "[CQ:type, first"
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
                    // "[CQ:type, first ", ignore spaces after param
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
                    // "[CQ:type, first =s",  value is always longer than 0
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
                    // "[CQ:type, first =second"
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
                    // "[CQ:type, first =second]"
                    // a well-formed cq code block
                    if (pos_of_lsbracket > panic_pos) {
                        panic_end(pos_of_lsbracket);
                    }
                    container.emplace_back(std::move(temp_item));
                    operation_pos = panic_pos = i + 1;
                    break;
                } else if (mode == CQ_BLOCK_DETECT_TYPE::none) {
                    // ill-formed
                    break;
                }
            }
        }
    }

    Message::Message(const string &msg_str) {
        // implement a DFA manually, because the regex lib of VC++ will throw stack overflow in some cases

        const static auto TEXT = 0;
        const static auto FUNCTION_NAME = 1;
        const static auto PARAMS = 2;
        auto state = TEXT;
        const auto end = msg_str.cend();
        stringstream text_s, function_name_s, params_s;
        auto curr_cq_start = end;

        std::list<cq_disassemblies> container;
        cq_disassemble(msg_str, container);

        for (auto const &item : container) {
            MessageSegment seg;
            seg.type = item.type;
            for (auto const &param : item.params) {
                seg.data[param.first] = param.second;
            }
            this->emplace_back(std::move(seg));
        }
    }

    Message::operator string() const {
        stringstream ss;
        for (auto seg : *this) {
            if (seg.type.empty()) {
                continue;
            }
            if (seg.type == "text") {
                if (const auto it = seg.data.find("text"); it != seg.data.end()) {
                    ss << cq_escape((*it).second, false);
                }
            } else {
                ss << "[CQ:" << seg.type;
                for (const auto &item : seg.data) {
                    ss << "," << item.first << "=" << cq_escape(item.second, true);
                }
                ss << "]";
            }
        }
        return ss.str();
    }

    int64_t Message::send(const Target &target) const { return api::send_msg(target, *this); }

    string Message::extract_plain_text() const {
        string result;
        for (const auto &seg : *this) {
            if (seg.type == "text") {
                result += seg.data.at("text") + " ";
            }
        }
        if (!result.empty()) {
            result.erase(result.end() - 1); // remove the trailing space
        }
        return result;
    }

    void Message::reduce() {
        if (this->empty()) {
            return;
        }

        auto last_seg_it = this->begin();
        for (auto it = this->begin(); ++it != this->end();) {
            if (it->type == "text" && last_seg_it->type == "text" && it->data.find("text") != it->data.end()
                && last_seg_it->data.find("text") != last_seg_it->data.end()) {
                // found adjacent "text" segments
                last_seg_it->data["text"] += it->data["text"];
                // remove the current element and continue
                this->erase(it);
                it = last_seg_it;
            } else {
                last_seg_it = it;
            }
        }

        if (this->size() == 1 && this->front().type == "text" && this->extract_plain_text().empty()) {
            this->clear(); // the only item is an empty text segment, we should remove it
        }
    }
} // namespace cq::message
