#pragma once

#include <string>
#include <vector>

namespace ktplace {

class ktReportTable {
public:
    explicit ktReportTable(std::string title);

    void setHeaders(std::vector<std::string> headers);

    void addRow(std::vector<std::string> cells);

    void add(std::string key, std::string value);

    void clear();

    [[nodiscard]] std::string render() const;

    void emit() const;

private:
    std::string title;
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

}  // namespace ktplace
