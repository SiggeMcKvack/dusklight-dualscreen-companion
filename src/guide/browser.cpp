// In-app browser for grabbing a guide page: a WebView overlay on the game activity, hosted by
// the GuideBrowser class in the mod's DEX (see java/). The JNI plumbing lives in jni_bridge.cpp.
#include "dusk/guide/browser.hpp"

#include "../jni_bridge.hpp"

namespace dusk::guide {

bool browser_available() {
    return dsc::jni::is_bootstrapped();
}

bool open_browser(const char* url) {
    return dsc::jni::open_guide_browser(url != nullptr ? url : kDefaultGuideUrl);
}

}  // namespace dusk::guide
