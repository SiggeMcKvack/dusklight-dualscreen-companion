// The fork's offline walkthrough guide needs an in-app browser Activity (a manifest entry a
// mod cannot add), so the reader is not ported. These keep the rest of the dashboard's guide
// hooks inert: no guide is ever available, so the Guide page never appears.
#include "dusk/companion_internal.h"

namespace dusk::companion {

f32 s_scrollGuide = 0.0f;

void drawGuideOverlay(f32, f32, f32, f32) {}
bool handleGuideTouch(f32, f32) {
    return false;
}
bool guideIsOpen() {
    return false;
}
void guideOpen() {}
void guideClose() {}
void guideBack() {}
void guideRowTap(int) {}
bool guideAvailable() {
    return false;
}
void drawLeftGuideBox(f32, f32, f32) {}

}  // namespace dusk::companion
