#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace {

inline constexpr std::string_view VERSION = "0.1.0";

class kt_option {
public:
    std::string inputPath;
    /// A Tcl script to run instead of the fixed flow; see tcl/kt_tclShell.h.
    /// With one, the input directory is optional: the script reads its design.
    std::string tclScript;
    /// Configuration scripts (-c), run in order before the flow or the -t
    /// script: add_option lines that set the stages' options.
    std::vector<std::string> configScripts;
    /// ktplace-gui only (-b): where its Runs panel looks for designs.
    std::string benchRoot;
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

    /// @p inputOptional: ktplace-gui's case, where a window may open with no
    /// design and no script.
    bool parse(int argc, char *argv[], bool inputOptional = false);

    void report() const;

    static void printUsage();

    /// With `--gui` anywhere on the command line: the other arguments, in
    /// order, for ktplace-gui. nullopt without it, and also when -h/--help or
    /// -V/--version is there, so those keep answering for ktplace itself.
    [[nodiscard]] static std::optional<std::vector<std::string>> guiArguments(int argc,
                                                                              char *argv[]);
    /// ktplace-gui, which is built beside ktplace: the running executable's
    /// directory (from /proc/self/exe, else from @p argv0), plus the name.
    [[nodiscard]] static std::filesystem::path guiExecutable(const char *argv0);
    static void printVersion();
};

}  // namespace ktplace
