/**
 * @file kt_option.h
 * @brief Command-line option parser for KTPlace
 */

#pragma once

#include <string>
#include <memory>

namespace ktplace {

/**
 * @brief Command-line option parser using PIMPL pattern
 * 
 * Handles parsing of command-line arguments and provides
 * access to configuration options.
 */
class kt_option {
public:
    /// Constructor
    kt_option();

    /// Destructor
    ~kt_option();

    // Copy semantics (deleted)
    kt_option(const kt_option &) = delete;
    kt_option &operator=(const kt_option &) = delete;

    // Move semantics
    kt_option(kt_option &&) noexcept;
    kt_option &operator=(kt_option &&) noexcept;

    /**
     * @brief Parse command-line arguments
     * After parsing, relative output and plot paths are resolved against the
     * work directory, and the log path defaults to "<work-dir>/ktplace.log"
     * (its trace companion lives next to it). Absolute paths are used as
     * given. The work directory itself defaults to the current directory.
     *
     * @param argc Argument count
     * @param argv Argument vector
     * @return true if parsing successful, false if help/version shown
     * @throws std::runtime_error if parsing fails
     */
    bool parse_option(int argc, char *argv[]);

    // Accessors for parsed options
    [[nodiscard]] const std::string &getInputBaseName() const;
    [[nodiscard]] const std::string &getInputDir() const;
    [[nodiscard]] const std::string &getOutputPath() const;
    [[nodiscard]] const std::string &getAlgorithm() const;
    [[nodiscard]] const std::string &getOutputFormat() const;
    [[nodiscard]] const std::string &getLogFile() const;
    [[nodiscard]] const std::string &getConfigFile() const;
    [[nodiscard]] const std::string &getPlotDir() const;

    [[nodiscard]] const std::string &getWorkDir() const;

    /**
     * @brief Check if verbose (trace-level) logging was requested
     * @return true if -v/--verbose was given
     */
    [[nodiscard]] bool isVerbose() const;

    /**
     * @brief Print usage information
     * @param programName Program name for usage output
     */
    static void printUsage(const char *programName);

    /**
     * @brief Print version information
     */
    static void printVersion();

private:
    /// Apply the work directory to the output, plot and log paths.
    void resolvePaths();

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
