#pragma once

#include <memory>

namespace ktplace {

class kt_option;

class FlowMgr {
public:
    FlowMgr();
    ~FlowMgr();

    FlowMgr(const FlowMgr &) = delete;
    FlowMgr &operator=(const FlowMgr &) = delete;
    FlowMgr(FlowMgr &&) noexcept;
    FlowMgr &operator=(FlowMgr &&) noexcept;

    void run(const kt_option &options);

    /// Load the run's configuration script (--tcl), if it has one: its
    /// add_option lines set the stages' options. run() does this first; a
    /// --script run calls it before its own script.
    static void configure(const kt_option &options);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
