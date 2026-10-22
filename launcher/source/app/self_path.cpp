#include <string>

#include "mkw/services.h"

namespace mkw {
namespace {

std::string& SelfPath() {
    static std::string path = kLauncherNroPath;
    return path;
}

}  // namespace

void SetLauncherSelfPath(const char* argv0) {
    // hbloader passes the NRO path as argv[0] ("sdmc:/switch/..."). Anything else keeps the default.
    if (argv0 == nullptr) return;
    const std::string p(argv0);
    if (p.rfind("sdmc:/", 0) == 0 && p.size() > 4 && p.compare(p.size() - 4, 4, ".nro") == 0) SelfPath() = p;
}

const std::string& LauncherSelfPath() { return SelfPath(); }

}  // namespace mkw
