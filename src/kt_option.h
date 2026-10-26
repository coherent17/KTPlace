#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ktplace {

inline constexpr std::string_view version = "1.1.0";

enum class Mode { Batch, Shell, Gui };

class kt_option {
public:
    std::string inputPath;
    std::string workDir = std::filesystem::current_path().string();
    std::string tclFile;
    std::string command;
    Mode mode = Mode::Batch;
    bool verbose = false;

    [[nodiscard]] std::string getOutputPath() const {
        return getInWorkDir("placed.pl");
    }
    [[nodiscard]] std::string getPlotDir() const {
        return getInWorkDir("plots");
    }
    [[nodiscard]] std::string getLogFile() const {
        return getInWorkDir("ktplace.log");
    }

    bool parse(int argc, char *argv[]);
    void report() const;

    static void printUsage();
    static void printVersion();

private:
    [[nodiscard]] std::string getInWorkDir(std::string_view name) const {
        return (std::filesystem::path(workDir) / name).string();
    }
};

}  // namespace ktplace
