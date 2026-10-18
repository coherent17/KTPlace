#pragma once

#include "gui/kt_layout.h"

#include <functional>
#include <memory>
#include <string>

class QObject;

namespace ktplace::gui {

struct RunRequest {
    std::string design;
    std::string workDir;
    std::string script;
};

struct RunResult {
    enum class Outcome { Placed, Evaluated, Stopped, Failed };
    Outcome outcome = Outcome::Failed;
    std::string message;
    bool fromConsole = false;
};

class Runner {
public:
    struct Callbacks {
        std::function<void(LayoutFrame)> frame;
        std::function<void(const RunResult &)> finished;
    };

    Runner(QObject *context, Callbacks callbacks);
    ~Runner();

    Runner(const Runner &) = delete;
    Runner &operator=(const Runner &) = delete;

    bool start(const RunRequest &request);
    bool execute(const std::string &command, const std::string &workDir);
    void stop();
    [[nodiscard]] bool running() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
