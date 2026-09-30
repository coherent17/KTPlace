// @file kt_reportTable.cc// Implementation of the aligned table renderer


#include "util/kt_reportTable.h"
#include "util/kt_log.h"

#include <algorithm>

namespace ktplace {

namespace {

/// Visible width of a UTF-8 string (box drawing marks are multi-byte).
std::size_t displayWidth(const std::string &text) {
    std::size_t width = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {  // skip UTF-8 continuation bytes
            ++width;
        }
    }
    return width;
}

/// True when every character could belong to a number, so the cell is right
/// aligned to keep digit columns visually aligned.
bool looksNumeric(const std::string &text) {
    if (text.empty()) {
        return false;
    }
    bool digit = false;
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c != '.' && c != '-' && c != '+' && c != ',' && c != 'e' && c != 'E' &&
                   c != 'x' && c != 'X' && c != ' ' && c != '%' && c != '/') {
            return false;
        }
    }
    return digit;
}

std::string pad(const std::string &text, std::size_t width, bool rightAlign) {
    const std::size_t len = displayWidth(text);
    if (len >= width) {
        return text;
    }
    const std::string fill(width - len, ' ');
    return rightAlign ? fill + text : text + fill;
}

}  // namespace

ktReportTable::ktReportTable(std::string tableTitle) : title(std::move(tableTitle)) {}

void ktReportTable::setHeaders(std::vector<std::string> headerCells) {
    headers = std::move(headerCells);
}

void ktReportTable::addRow(std::vector<std::string> cells) {
    rows.push_back(std::move(cells));
}

void ktReportTable::add(std::string key, std::string value) {
    rows.push_back({std::move(key), std::move(value)});
}

void ktReportTable::clear() {
    headers.clear();
    rows.clear();
}

std::string ktReportTable::render() const {
    // Column count: the widest row wins.
    std::size_t columns = headers.size();
    for (const auto &row : rows) {
        columns = std::max(columns, row.size());
    }
    if (columns == 0) {
        return {};
    }

    // Measure every column, treating a missing cell as empty.
    std::vector<std::size_t> widths(columns, 0);
    const auto measure = [&](const std::vector<std::string> &cells) {
        for (std::size_t i = 0; i < columns; ++i) {
            const std::string &cell = i < cells.size() ? cells[i] : std::string();
            widths[i] = std::max(widths[i], displayWidth(cell));
        }
    };
    if (!headers.empty()) {
        measure(headers);
    }
    for (const auto &row : rows) {
        measure(row);
    }

    std::vector<bool> numeric(columns, false);
    for (std::size_t i = 0; i < columns; ++i) {
        // A column is right aligned when every non-empty body cell is numeric.
        bool allNumeric = true;
        bool any = false;
        for (const auto &row : rows) {
            if (i >= row.size() || row[i].empty()) {
                continue;
            }
            any = true;
            if (!looksNumeric(row[i])) {
                allNumeric = false;
                break;
            }
        }
        numeric[i] = any && allNumeric;
    }

    std::string out;
    const auto rule = [&](const char *left, const char *mid, const char *right) {
        out += left;
        for (std::size_t i = 0; i < columns; ++i) {
            if (i > 0) {
                out += mid;
            }
            out.append(widths[i] + 2, '-');
        }
        out += right;
        out += '\n';
    };
    const auto line = [&](const std::vector<std::string> &cells) {
        out += '|';
        for (std::size_t i = 0; i < columns; ++i) {
            const std::string empty;
            const std::string &cell = i < cells.size() ? cells[i] : empty;
            out += ' ';
            out += pad(cell, widths[i], numeric[i]);
            out += " |";
        }
        out += '\n';
    };

    if (!title.empty()) {
        out += title;
        out += '\n';
    }
    rule("+", "+", "+");
    if (!headers.empty()) {
        line(headers);
        rule("+", "+", "+");
    }
    for (const auto &row : rows) {
        line(row);
    }
    rule("+", "+", "+");
    return out;
}

namespace {
/// render() ends with a newline; the logger adds its own record terminator,
/// so drop it to avoid a blank line after the table.
std::string withoutTrailingNewline(std::string text) {
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}
}  // namespace

void ktReportTable::emit() const {
    const std::string text = withoutTrailingNewline(render());
    if (!text.empty()) {
        ktlog.echo("{}", text);
    }
}
}  // namespace ktplace
