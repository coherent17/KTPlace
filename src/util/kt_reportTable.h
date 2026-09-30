// @file kt_reportTable.h// Aligned text tables, rendered through the logger// A small builder for the summary tables the engine prints at the end of a// run (netlist statistics, solver results, phase timings). It measures the// content to size every column, right-aligns cells that look numeric so digits// line up, and hands the finished block to `ktlog` in one record rather than a// stream of separately timestamped lines.// Cells are always explicit, so text is never re-flowed: a row is either one// formatted cell (`row`) or a list of them (`addRow`).// Usage:// ktReportTable table("Netlist");// table.setHeaders({"metric", "value"});// table.add("cells", fmt::format("{}", numCells));// table.addRow({"nets", fmt::format("{}", numNets)});// table.emit();          // echo level, through ktlog// table.emitVerbose();   // trace level, through ktlog


#pragma once

#include <fmt/format.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

class ktReportTable {
public:
    /// Start a table with a title line.
    explicit ktReportTable(std::string title);

    /// Replace the header row; omit this for a table without headers.
    void setHeaders(std::vector<std::string> headers);

    /// Append one row of already-formatted cells.
    void addRow(std::vector<std::string> cells);

    /// Append a single-cell row built from a `fmt::format` string.
    template <typename... Args>
    void row(fmt::format_string<Args...> fmtStr, Args &&...args) {
        addRow({fmt::format(fmtStr, std::forward<Args>(args)...)});
    }

    /// Append a two-column `key | value` row.
    void add(std::string key, std::string value);

    /// Drop every row and header, keeping the title.
    void clear();

    /// Render the table as text, without any log prefix.
    [[nodiscard]] std::string render() const;

    /// Render and write the table through `ktlog::echo` as a single record.
    void emit() const;

private:
    std::string title;
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

}  // namespace ktplace
