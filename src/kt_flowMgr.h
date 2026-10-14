#pragma once

#include <memory>
#include <string>

namespace ktplace {

class kt_option;

/// The trace a run started with --record writes under its plot directory.
[[nodiscard]] std::string traceFile(const std::string &plotDir);

class FlowMgr {
public:
    FlowMgr();
    ~FlowMgr();

    FlowMgr(const FlowMgr &) = delete;
    FlowMgr &operator=(const FlowMgr &) = delete;
    FlowMgr(FlowMgr &&) noexcept;
    FlowMgr &operator=(FlowMgr &&) noexcept;

    void run(const kt_option &options);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
