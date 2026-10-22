#pragma once
#include <string_view>

namespace RecompMod {
using ProfileInitializerFn = void (*)();
inline void RegisterProfileInitializer(std::string_view, ProfileInitializerFn fn) {
    if (fn) {
        fn();
    }
}
inline void ActivateProfile(std::string_view) {}
} // namespace RecompMod
