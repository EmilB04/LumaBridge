#pragma once

#include <memory>

namespace luma::app {

// Call after ImGui::Render(), before the renderer consumes its draw data.
// Closing windows leave only a short-lived drawing; input closes immediately.
class PopupAnimations {
public:
    PopupAnimations();
    ~PopupAnimations();
    void Render();
    void Reset();  // before replacing fonts or other UI textures

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace luma::app
