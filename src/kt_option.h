#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ktplace {

inline constexpr std::string_view VERSION = "0.1.0";

class kt_option {
public:
    std::string inputPath;
    std::string algorithm = "simpl";
    bool verbose = false;
    std::string workDir = std::filesystem::current_path().string();

    std::string getOutputPath() const {
        return (std::filesystem::path(workDir) / "placed.pl").string();
    }

    std::string getPlotDir() const {
        return (std::filesystem::path(workDir) / "plots").string();
    }

    std::string getLogFile() const {
        return (std::filesystem::path(workDir) / "ktplace.log").string();
    }

    bool parse(int argc, char *argv[]);

    void report() const;

    static void printUsage();
    static void printVersion();
};

}  // namespace ktplace
