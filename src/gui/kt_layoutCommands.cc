#include "gui/kt_layoutCommands.h"

#include "gui/kt_displayStyle.h"
#include "gui/kt_layout.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_namePattern.h"
#include "gui/kt_selectionSet.h"
#include "tcl/kt_tcl.h"
#include "tcl/kt_tclMgr.h"

#include <QImage>
#include <QMetaObject>
#include <QString>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ktplace::gui {

namespace {

using Words = std::vector<std::string>;
using Body = std::function<tcl::Value(LayoutView &, const Words &)>;

constexpr auto answerPoll = std::chrono::milliseconds(50);
constexpr std::size_t unlimited = std::numeric_limits<std::size_t>::max();

std::atomic<LayoutView *> bound{nullptr};

struct Arity {
    std::size_t min = 0;
    std::size_t max = 0;
};

tcl::Value onViewThread(const std::string &command, Body body, Words words) {
    LayoutView *view = bound.load();
    if (view == nullptr) {
        throw tcl::Error(std::format("{}: no layout window is open", command));
    }
    if (QThread::currentThread() == view->thread()) {
        return body(*view, words);
    }
    auto answer = std::make_shared<std::promise<tcl::Value>>();
    std::future<tcl::Value> result = answer->get_future();
    QMetaObject::invokeMethod(
        view,
        [answer, command, body = std::move(body), words = std::move(words)] {
            try {
                LayoutView *live = bound.load();
                if (live == nullptr) {
                    throw tcl::Error(std::format("{}: the layout window closed", command));
                }
                answer->set_value(body(*live, words));
            } catch (...) {
                answer->set_exception(std::current_exception());
            }
        },
        Qt::QueuedConnection);
    while (result.wait_for(answerPoll) != std::future_status::ready) {
        if (bound.load() == nullptr) {
            throw tcl::Error(std::format("{}: the layout window closed", command));
        }
    }
    return result.get();
}

void registerLayoutCommand(tcl::CommandHelp help, Arity arity, Body body) {
    tcl::TclMgr::instance().registerCommand(
        help, [help, arity, body = std::move(body)](
                  tcl::Interpreter &interp, const tcl::Arguments &args, tcl::Value &result) {
            const std::size_t given = args.size() - 1;
            if (given < arity.min || given > arity.max) {
                throw tcl::Error(std::format("wrong # args: should be \"{}\"", help.usage));
            }
            Words words;
            words.reserve(given);
            for (std::size_t i = 1; i < args.size(); ++i) {
                words.push_back(args[i].literal ? args[i].text : interp.substitute(args[i].text));
            }
            try {
                result = onViewThread(help.name, body, std::move(words));
            } catch (const std::logic_error &error) {
                throw tcl::Error(std::format("{}: {}", help.name, error.what()));
            }
            return 0;
        });
}

const LayoutFrame &shownFrame(const LayoutView &view) {
    if (!view.frame().drawable()) {
        throw std::logic_error("no design is shown; read or open one first");
    }
    return view.frame();
}

std::vector<std::uint32_t> matchAll(const std::vector<std::string> &names,
                                    std::span<const std::string> patterns) {
    std::vector<std::uint32_t> found;
    for (const std::string &pattern : patterns) {
        const std::vector<std::uint32_t> some = matchNames(names, NamePattern(pattern));
        found.insert(found.end(), some.begin(), some.end());
    }
    std::ranges::sort(found);
    const auto [first, last] = std::ranges::unique(found);
    found.erase(first, last);
    return found;
}

double coordinate(const std::string &word) {
    std::size_t used = 0;
    double value = 0.0;
    try {
        value = std::stod(word, &used);
    } catch (const std::exception &) {
        used = 0;
    }
    if (used != word.size() || word.empty()) {
        throw std::logic_error(std::format("expected a number but got \"{}\"", word));
    }
    return value;
}

bool switchValue(const std::string &word) {
    if (word == "on" || word == "1" || word == "true" || word == "yes") {
        return true;
    }
    if (word == "off" || word == "0" || word == "false" || word == "no") {
        return false;
    }
    throw std::logic_error(std::format("expected on or off but got \"{}\"", word));
}

DisplayLayer layerNamed(const std::string &word) {
    if (const std::optional<DisplayLayer> layer = DisplayStyle::fromKey(word)) {
        return *layer;
    }
    std::string known;
    for (const DisplayLayer layer : displayLayers) {
        known += std::format("{}{}", known.empty() ? "" : ", ", DisplayStyle::key(layer));
    }
    throw std::logic_error(std::format("unknown layer \"{}\"; known layers: {}", word, known));
}

void registerCommands() {
    registerLayoutCommand(
        {.name = "layout_select",
         .usage = "layout_select ?-add? pattern ?pattern ...?",
         .summary = "select the cells whose names match; returns how many matched"},
        {.min = 1, .max = unlimited}, [](LayoutView &view, const Words &words) {
            const LayoutFrame &frame = shownFrame(view);
            const bool add = words.front() == "-add";
            const std::span<const std::string> patterns = std::span(words).subspan(add ? 1 : 0);
            if (patterns.empty()) {
                throw std::logic_error("give at least one pattern");
            }
            std::vector<std::uint32_t> found = matchAll(frame.geometry->name, patterns);
            const auto count = static_cast<std::int64_t>(found.size());
            if (add) {
                view.selection().addCells(found);
            } else {
                view.selection().selectCells(std::move(found));
            }
            return tcl::Value::integer(count);
        });
    registerLayoutCommand({.name = "layout_mark_nets",
                           .usage = "layout_mark_nets pattern ?pattern ...?",
                           .summary = "mark the nets whose names match; returns how many matched"},
                          {.min = 1, .max = unlimited}, [](LayoutView &view, const Words &words) {
                              const LayoutFrame &frame = shownFrame(view);
                              std::vector<std::uint32_t> found =
                                  matchAll(frame.geometry->netName, words);
                              const auto count = static_cast<std::int64_t>(found.size());
                              view.selection().markNets(std::move(found));
                              return tcl::Value::integer(count);
                          });
    registerLayoutCommand(
        {.name = "layout_connectivity",
         .usage = "layout_connectivity ?on|off?",
         .summary = "show or hide the nets of the selected cells; returns how many are shown"},
        {.min = 0, .max = 1}, [](LayoutView &view, const Words &words) {
            shownFrame(view);
            if (words.empty() || switchValue(words.front())) {
                view.showConnectivity();
            } else {
                view.hideConnectivity();
            }
            return tcl::Value::integer(static_cast<std::int64_t>(view.selection().nets().size()));
        });
    registerLayoutCommand({.name = "layout_clear",
                           .usage = "layout_clear",
                           .summary = "drop the selected cells and the marked nets"},
                          {}, [](LayoutView &view, const Words &) {
                              view.selection().clear();
                              return tcl::Value{};
                          });
    registerLayoutCommand(
        {.name = "layout_view_all", .usage = "layout_view_all", .summary = "show the whole die"},
        {}, [](LayoutView &view, const Words &) {
            view.zoomFit();
            return tcl::Value{};
        });
    registerLayoutCommand(
        {.name = "layout_view_area",
         .usage = "layout_view_area x1 y1 x2 y2",
         .summary = "show the area between two corners, in design units"},
        {.min = 4, .max = 4}, [](LayoutView &view, const Words &words) {
            const double x1 = coordinate(words[0]);
            const double y1 = coordinate(words[1]);
            const double x2 = coordinate(words[2]);
            const double y2 = coordinate(words[3]);
            view.zoomTo({std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2)});
            return tcl::Value{};
        });
    registerLayoutCommand(
        {.name = "layout_view_selection",
         .usage = "layout_view_selection",
         .summary = "show the selected cells and marked nets; returns 1 if there were any"},
        {}, [](LayoutView &view, const Words &) {
            return tcl::Value::boolean(view.zoomToSelection());
        });
    registerLayoutCommand(
        {.name = "layout_layer",
         .usage = "layout_layer ?layer? ?on|off?",
         .summary = "list the layers, tell whether one is shown, or show or hide it"},
        {.min = 0, .max = 2}, [](LayoutView &view, const Words &words) {
            if (words.empty()) {
                std::vector<tcl::Value> layers;
                for (const DisplayLayer layer : displayLayers) {
                    layers.push_back(
                        tcl::Value::string(std::format("{} {}", DisplayStyle::key(layer),
                                                       view.style().shows(layer) ? "on" : "off")));
                }
                return tcl::Value::list(std::move(layers));
            }
            const DisplayLayer layer = layerNamed(words[0]);
            if (words.size() == 2) {
                DisplayStyle next = view.style();
                next[layer].visible = switchValue(words[1]);
                view.setStyle(next);
            }
            return tcl::Value::boolean(view.style().shows(layer));
        });
    registerLayoutCommand(
        {.name = "layout_capture",
         .usage = "layout_capture file.png",
         .summary = "save the layout view as an image; returns the file"},
        {.min = 1, .max = 1}, [](LayoutView &view, const Words &words) {
            if (!view.capture().save(QString::fromStdString(words[0]))) {
                throw std::logic_error(std::format("cannot write \"{}\"", words[0]));
            }
            return tcl::Value::string(words[0]);
        });
}

}  // namespace

LayoutCommandBinding::LayoutCommandBinding(LayoutView &target) : view(target) {
    static const bool registered = (registerCommands(), true);
    (void)registered;
    bound.store(&view);
}

LayoutCommandBinding::~LayoutCommandBinding() {
    LayoutView *expected = &view;
    bound.compare_exchange_strong(expected, nullptr);
}

}  // namespace ktplace::gui
