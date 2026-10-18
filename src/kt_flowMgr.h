#pragma once

#include <functional>
#include <memory>
#include <string>
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

    void begin(const kt_option &options);
    [[nodiscard]] std::string evaluate(const std::string &script);
    void finish();
    [[nodiscard]] bool hasDesign() const;

    void readDesign(const std::string &dirPath);
    void globalPlace();
    void legalize();
    void detailPlace();
    std::string writePlacement(const std::string &path = {});
    void writePlots();
    [[nodiscard]] double hpwl() const;
    void reportDesign() const;
    [[nodiscard]] bool checkPlacement() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
