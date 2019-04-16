#include "./message.h"

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

    string escape(string s, const bool escape_comma = true) {
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

    Message::Message(const string &msg_str) {
        // implement a DFA manually, because the regex lib of VC++ will throw stack overflow in some cases

        const static auto TEXT = 0;
        const static auto FUNCTION_NAME = 1;
        const static auto PARAMS = 2;
        auto state = TEXT;
        const auto end = msg_str.cend();
        stringstream text_s, function_name_s, params_s;
        auto curr_cq_start = end;

        std::list<sutils::cq_disasemblies> container;
        sutils::cq_disasemble(msg_str, container);

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
