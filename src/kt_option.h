#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ktplace {

inline constexpr std::string_view version = "1.0.0";

class kt_option {
public:
    std::string inputPath;
    bool verbose = false;
    std::string workDir = std::filesystem::current_path().string();

    std::string getOutputPath() const {
        return (std::filesystem::path(workDir) / "placed.pl").string();
    }

    std::string getPlotDir() const {
        return plot ? (std::filesystem::path(workDir) / "plots").string() : std::string();
    }

    std::string getLogFile() const {
        return (std::filesystem::path(workDir) / "ktplace.log").string();
    }

    bool plot = true;
    std::string tclFile;
    bool gui = false;

    bool parse(int argc, char *argv[]);

    void report() const;

    static void printUsage();
    static void printVersion();
};

}  // namespace ktplace
