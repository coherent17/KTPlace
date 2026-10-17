#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace ktplace {

class kt_option;
class ktDM;

struct FlowObserver {
    std::function<void(std::string_view stage, const ktDM &db)> committed;
    std::function<void(std::string_view stage, const std::vector<float> &x,
                       const std::vector<float> &y, double hpwl)>
        frame;
};

class FlowMgr {
public:
    FlowMgr();
    ~FlowMgr();

    FlowMgr(const FlowMgr &) = delete;
    FlowMgr &operator=(const FlowMgr &) = delete;
    FlowMgr(FlowMgr &&) noexcept;
    FlowMgr &operator=(FlowMgr &&) noexcept;

    void run(const kt_option &options);

    void observe(FlowObserver observer);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
