/**
 * @file kt_option.h
 * @brief Command-line option parser for KTPlace
 */

#ifndef KT_OPTION_H
#define KT_OPTION_H

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

    /**
     * @brief Check if help was requested
     * @return true if help was requested
     */
    [[nodiscard]] bool isHelpRequested() const;

    /**
     * @brief Check if verbose (trace-level) logging was requested
     * @return true if -v/--verbose was given
     */
    [[nodiscard]] bool isVerbose() const;

    /**
     * @brief Check if version was requested
     * @return true if version was requested
     */
    [[nodiscard]] bool isVersionRequested() const;

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
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace

#endif  // KT_OPTION_H
