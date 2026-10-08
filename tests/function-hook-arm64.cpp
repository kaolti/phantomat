#include "FunctionHook.hpp"
#include <cstdio>
#include <cstdlib>

static volatile int bias = 7;
static int (*original)(int) = nullptr;

__attribute__((noinline)) static int target(int value) { return value + bias; }
static int replacement(int value) { return original(value) + 100; }

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
    std::printf("PASS %s\n", message);
}

int main() {
    int (*volatile call)(int) = target;
    COverviewFunctionHook invalid(nullptr, reinterpret_cast<void*>(replacement));
    check(!invalid.hook(), "reject a missing target");
    check(invalid.unhook(), "inactive removal succeeds");
    check(call(3) == 10, "baseline target");
    for (int cycle = 0; cycle < 5; ++cycle) {
        {
            COverviewFunctionHook hook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement));
            check(hook.hook(), "install hook");
            original = reinterpret_cast<int (*)(int)>(hook.m_original);
            check(call(3) == 110, "replacement calls relocated original");
            check(hook.hook(), "repeated install is harmless");
            check(call(3) == 110, "repeated install preserves the original trampoline");
            check(hook.unhook(), "remove hook");
            check(hook.m_original == nullptr && call(3) == 10, "removal clears the trampoline and restores target");
            check(hook.unhook(), "repeated removal is harmless");
            check(hook.hook(), "reinstall hook");
            original = reinterpret_cast<int (*)(int)>(hook.m_original);
            check(call(3) == 110, "reinstalled replacement works");
        }
        check(call(3) == 10, "destruction restores target");
        auto* owned = createOverviewFunctionHook(nullptr, reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement));
        check(owned->hook(), "install an owned hook");
        original = reinterpret_cast<int (*)(int)>(owned->m_original);
        check(call(3) == 110, "owned replacement works");
        releaseOverviewFunctionHooks();
        check(call(3) == 10, "release restores all owned hooks");
    }
}
