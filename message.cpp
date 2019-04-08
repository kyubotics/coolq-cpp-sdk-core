#include "./message.h"

#include <sstream>

#include "./api.h"

using namespace std;

namespace cq::message {
    /*
    string escape(string str, const bool escape_comma) {
        boost::replace_all(str, "&", "&amp;");
        boost::replace_all(str, "[", "&#91;");
        boost::replace_all(str, "]", "&#93;");
        if (escape_comma) boost::replace_all(str, ",", "&#44;");
        return str;
    }

    string unescape(string str) {
        boost::replace_all(str, "&#44;", ",");
        boost::replace_all(str, "&#91;", "[");
        boost::replace_all(str, "&#93;", "]");
        boost::replace_all(str, "&amp;", "&");
        return str;
    }
    */

    Message::Message(const string &msg_str) {
        // implement a DFA manually, because the regex lib of VC++ will throw stack overflow in some cases

        const static auto TEXT = 0;
        const static auto FUNCTION_NAME = 1;
        const static auto PARAMS = 2;
        auto state = TEXT;
        const auto end = msg_str.cend();
        stringstream text_s, function_name_s, params_s;
        auto curr_cq_start = end;

        std::vector<sutils::cq_disasemblies> container;
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
                    ss << sutils::cq_escape((*it).second, false);
                }
            } else {
                ss << "[CQ:" << seg.type;
                for (const auto &item : seg.data) {
                    ss << "," << item.first << "=" << sutils::cq_escape(item.second, true);
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
