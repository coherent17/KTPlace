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

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
