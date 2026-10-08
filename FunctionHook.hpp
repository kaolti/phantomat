#pragma once

#if defined(__aarch64__)
#include <dobby.h>
#include <memory>
#include <vector>

// Hyprland 0.56's function-hook backend only implements x86_64. Keep the
// architecture-specific trampoline/relocation work in Dobby on ARM64, while
// exposing the original-pointer interface used by the existing render hooks.
class COverviewFunctionHook {
  public:
    COverviewFunctionHook(void* source, void* destination) : m_source(source), m_destination(destination) {}
    ~COverviewFunctionHook() { unhook(); }

    COverviewFunctionHook(const COverviewFunctionHook&) = delete;
    COverviewFunctionHook& operator=(const COverviewFunctionHook&) = delete;

    bool hook() {
        if (m_active)
            return true;
        if (!m_source || !m_destination)
            return false;
        void* original = nullptr;
        if (DobbyHook(m_source, m_destination, &original) != 0 || !original)
            return false;
        m_original = original;
        m_active = true;
        return true;
    }

    bool unhook() {
        if (!m_active)
            return true;
        if (DobbyDestroy(m_source) != 0)
            return false;
        m_active = false;
        m_original = nullptr;
        return true;
    }

    void* m_original = nullptr;

  private:
    void* m_source = nullptr;
    void* m_destination = nullptr;
    bool m_active = false;
};

inline std::vector<std::unique_ptr<COverviewFunctionHook>> g_overviewFunctionHooks;

inline COverviewFunctionHook* createOverviewFunctionHook(void*, void* source, void* destination) {
    auto hook = std::make_unique<COverviewFunctionHook>(source, destination);
    auto* result = hook.get();
    g_overviewFunctionHooks.emplace_back(std::move(hook));
    return result;
}

inline void releaseOverviewFunctionHooks() {
    g_overviewFunctionHooks.clear();
}
#else
#include <hyprland/src/plugins/PluginAPI.hpp>

using COverviewFunctionHook = CFunctionHook;

inline COverviewFunctionHook* createOverviewFunctionHook(HANDLE owner, void* source, void* destination) {
    return HyprlandAPI::createFunctionHook(owner, source, destination);
}

inline void releaseOverviewFunctionHooks() {}
#endif
