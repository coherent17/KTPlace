#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ktplace {

inline constexpr std::string_view VERSION = "0.1.0";

class kt_option {
public:
    std::string inputPath;
    /// A Tcl script to run instead of the fixed flow; see tcl/kt_tclShell.h.
    /// With one, the input directory is optional: the script reads its design.
    std::string tclScript;
    std::string algorithm = "simpl";
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

    // Whether to draw at all. The picture settings themselves are defaults that
    // the environment can override; see PlotOptions.
    bool plot = true;

    bool parse(int argc, char *argv[]);

    void report() const;

    static void printUsage();
    static void printVersion();
};

}  // namespace ktplace
