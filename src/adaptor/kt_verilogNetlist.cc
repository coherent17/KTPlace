#include "adaptor/kt_verilogNetlist.h"

#include "util/kt_gzipFile.h"

#include <cctype>
#include <cstddef>
#include <format>
#include <istream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ktplace {

namespace {

class Statements {
public:
    explicit Statements(std::istream &in) : stream(in) {}

    bool next(std::vector<std::string> &words) {
        words.clear();
        std::string word;
        char c = 0;
        while (stream.get(c)) {
            if (c == '/' && stream.peek() == '/') {
                std::string comment;
                std::getline(stream, comment);
                continue;
            }
            if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                flush(word, words);
                if (!words.empty() && words.back() == "endmodule") {
                    return true;
                }
                continue;
            }
            if (c == '(' || c == ')' || c == ',' || c == '.') {
                flush(word, words);
                words.emplace_back(1, c);
                continue;
            }
            if (c == ';') {
                flush(word, words);
                return true;
            }
            word += c;
        }
        flush(word, words);
        return !words.empty();
    }

private:
    std::istream &stream;

    static void flush(std::string &word, std::vector<std::string> &words) {
        if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
};

class Netlist {
public:
    VerilogNet &net(const std::string &name) {
        const auto [it, added] = index.try_emplace(name, nets.size());
        if (added) {
            nets.push_back({.name = name, .port = false, .pins = {}});
        }
        return nets[it->second];
    }

    std::vector<VerilogNet> release() {
        return std::move(nets);
    }

private:
    std::unordered_map<std::string, std::size_t> index;
    std::vector<VerilogNet> nets;
};

void readInstance(const std::vector<std::string> &words, const std::string &path,
                  Netlist &netlist) {
    if (words.size() < 3 || words[2] != "(") {
        throw std::invalid_argument(
            std::format("{}: cannot read the statement starting '{}'", path, words.front()));
    }
    const std::string &inst = words[1];
    for (std::size_t i = 3; i + 4 < words.size(); ++i) {
        if (words[i] != "." || words[i + 2] != "(") {
            continue;
        }
        if (words[i + 3] != ")") {
            netlist.net(words[i + 3]).pins.push_back({.inst = inst, .pin = words[i + 1]});
        }
        i += 3;
    }
}

}  // namespace

std::vector<VerilogNet> readVerilogNetlist(const std::string &path) {
    GzipInputFile in(path);
    if (!in) {
        throw std::invalid_argument(std::format("cannot read {}", path));
    }
    Statements statements(in);
    Netlist netlist;
    std::vector<std::string> words;
    while (statements.next(words)) {
        if (words.empty()) {
            continue;
        }
        const std::string &head = words.front();
        if (head == "module" || head == "endmodule" || head == "wire" || head == "assign") {
            continue;
        }
        if (head == "input" || head == "output" || head == "inout") {
            for (std::size_t i = 1; i < words.size(); ++i) {
                if (words[i] != ",") {
                    netlist.net(words[i]).port = true;
                }
            }
            continue;
        }
        readInstance(words, path, netlist);
    }
    return netlist.release();
}

}  // namespace ktplace
