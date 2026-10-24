#pragma once

namespace ktplace::gui {

class LayoutView;

class LayoutCommandBinding {
public:
    explicit LayoutCommandBinding(LayoutView &view);
    ~LayoutCommandBinding();

    LayoutCommandBinding(const LayoutCommandBinding &) = delete;
    LayoutCommandBinding &operator=(const LayoutCommandBinding &) = delete;

private:
    LayoutView &view;
};

}  // namespace ktplace::gui
