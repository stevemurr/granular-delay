#pragma once
#include <cstdint>

namespace granular {
struct EditorCallbacks {
    void* context = nullptr;
    double (*get)(void*, uint32_t) = nullptr;
    bool (*edit)(void*, uint32_t, double, int) = nullptr; // 0 value, 1 gesture begin, 2 gesture end
    // 0 input peak, 1 output peak, 2 loop energy; each roughly 0..1 (peak-held, decaying).
    float (*meter)(void*, unsigned) = nullptr;
    double (*tempo)(void*) = nullptr;
};
class Editor {
public:
    virtual ~Editor() = default;
    virtual bool setParent(void* view) = 0;
    virtual void show(bool visible) = 0;
    virtual void* nativeView() const = 0;
    static Editor* create(EditorCallbacks callbacks);
    static constexpr uint32_t width = 760, height = 520;
};
} // namespace granular
