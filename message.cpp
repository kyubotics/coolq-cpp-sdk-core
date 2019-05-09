#include "./message.h"

#include <boost/algorithm/string.hpp>
#include <sstream>

#include "./api.h"

using namespace std;

namespace cq::message {
    const static unordered_map<string, char> UNESCAPE_MAP = {
        {"&amp;", '&'},
        {"&#91;", '['},
        {"&#93;", ']'},
        {"&#44;", ','},
    };

    constexpr static short MAX_ESCAPE_LEN = 5;

    string escape(string s, const bool escape_comma) {
        vector<char> out;
        for (const auto &ch : s) {
            string escaped;
            switch (ch) {
            case '&':
                escaped = "&amp;";
                break;
            case '[':
                escaped = "&#91;";
                break;
            case ']':
                escaped = "&#93;";
                break;
            case ',':
                if (escape_comma) escaped = "&#44;";
                break;
            }
            if (!escaped.empty()) {
                copy(escaped.begin(), escaped.end(), back_inserter(out));
            } else {
                out.push_back(ch);
            }
        }
        out.push_back('\0');
        return out.data();
    }

    string unescape(string s) {
        vector<char> out;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] != '&') {
                // it's not an escaped char
                out.push_back(s[i++]);
                continue;
            }
            size_t next_i = i + 1;
            vector<char> entity = {'&'};
            for (auto j = 1; j < MAX_ESCAPE_LEN && (isalnum(s[i + j]) || s[i + j] == '#' || s[i + j] == ';');
                 j++, next_i++) {
                entity.push_back(s[i + j]);
                if (s[i + j] == ';') {
                    next_i++;
                    break;
                }
            }
            if (!entity.empty()) {
                entity.push_back('\0');
                const string escaped = entity.data();
                if (UNESCAPE_MAP.count(escaped)) {
                    out.push_back(UNESCAPE_MAP.at(escaped));

                } else {
                    copy(entity.begin(), entity.end() - 1 /* omit the terminator */, back_inserter(out));
                }
            }
            i = next_i;
        }
        out.push_back('\0');
        return out.data();
    }

    // Message::Message(const string &msg_str) {
    //     // implement a DFA manually, because the regex lib of VC++ will throw stack overflow in some cases

    //     const static auto TEXT = 0;
    //     const static auto FUNCTION_NAME = 1;
    //     const static auto PARAMS = 2;
    //     auto state = TEXT;
    //     const auto end = msg_str.cend();
    //     stringstream text_s, function_name_s, params_s;
    //     auto curr_cq_start = end;

    //     std::list<sutils::cq_disassemblies> container;
    //     sutils::cq_disassemble(msg_str, container);

    //     for (auto const &item : container) {
    //         MessageSegment seg;
    //         seg.type = item.type;
    //         for (auto const &param : item.params) {
    //             seg.data[param.first] = param.second;
    //         }
    //         this->emplace_back(std::move(seg));
    //     }
    // }

    Message::Message(const string &msg_str) {
        // implement a DFA manually, because the regex lib of VC++ will throw stack overflow in some cases

        const static auto TEXT = 0;
        const static auto FUNCTION_NAME = 1;
        const static auto PARAMS = 2;
        auto state = TEXT;
        const auto end = msg_str.cend();
        stringstream text_s, function_name_s, params_s;
        auto curr_cq_start = end;
        for (auto it = msg_str.cbegin(); it != end; ++it) {
            const auto curr = *it;
            switch (state) {
            case TEXT: {
            text:
                if (curr == '[' && end - 1 - it >= 5 /* [CQ:a] at least 5 chars behind */
                    && *(it + 1) == 'C' && *(it + 2) == 'Q' && *(it + 3) == ':' && *(it + 4) != ']') {
                    state = FUNCTION_NAME;
                    curr_cq_start = it;
                    it += 3;
                } else {
                    text_s << curr;
                }
                break;
            }
            case FUNCTION_NAME: {
                if ((curr >= 'A' && curr <= 'Z') || (curr >= 'a' && curr <= 'z') || (curr >= '0' && curr <= '9')) {
                    function_name_s << curr;
                } else if (curr == ',') {
                    // function name out, params in
                    state = PARAMS;
                } else if (curr == ']') {
                    // CQ code end, with no params
                    goto params;
                } else {
                    // unrecognized character
                    text_s << string(curr_cq_start, it); // mark as text
                    curr_cq_start = end;
                    function_name_s = stringstream();
                    params_s = stringstream();
                    state = TEXT;
                    // because the current char may be '[', we goto text part
                    goto text;
                }
                break;
            }
            case PARAMS: {
            params:
                if (curr == ']') {
                    // CQ code end
                    MessageSegment seg;

                    seg.type = function_name_s.str();

                    vector<string> params;
                    utils::string_split(params, params_s.str(), ',');
                    for (const auto &param : params) {
                        const auto idx = param.find_first_of('=');
                        if (idx != string::npos) {
                            seg.data[boost::trim_copy(param.substr(0, idx))] = unescape(param.substr(idx + 1));
                        }
                    }

                    if (!text_s.str().empty()) {
                        // there is a text segment before this CQ code
                        this->push_back(MessageSegment{"text", {{"text", unescape(text_s.str())}}});
                        text_s = stringstream();
                    }

                    this->push_back(seg);
                    curr_cq_start = end;
                    text_s = stringstream();
                    function_name_s = stringstream();
                    params_s = stringstream();
                    state = TEXT;
                } else {
                    params_s << curr;
                }
            }
            default:
                break;
            }
        }

        // iterator end, there may be some rest of message we haven't put into segments
        switch (state) {
        case FUNCTION_NAME:
        case PARAMS:
            // we are in CQ code, but it ended with no ']', so it's a text segment
            text_s << string(curr_cq_start, end);
            // should fall through
        case TEXT:
            if (!text_s.str().empty()) {
                this->push_back(MessageSegment{"text", {{"text", unescape(text_s.str())}}});
            }
        default:
            break;
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
                    ss << escape((*it).second, false);
                }
            } else {
                ss << "[CQ:" << seg.type;
                for (const auto &item : seg.data) {
                    ss << "," << item.first << "=" << escape(item.second, true);
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
