#pragma once

// Background import of saved guide pages into the store. Pages come from the
// in-app WebView (browser.hpp), since walkthrough sites refuse non-browser
// clients; nothing here touches the network.

#include "dusk/guide/store.hpp"
#include <string>

namespace dusk::guide {

// --- Import, off the game thread ---
//
// scan_import_folder() converts every page and decodes its images, far too
// slow for the thread that draws, so it runs on a worker and the reader polls.
void begin_import();
bool import_in_progress();
// A finished import is observed by both the reader and the settings pane, on
// different screens and frames, so it is a monotonic generation rather than a
// one-shot flag: each consumer remembers the last value it acted on. Bumped
// once per completed import; reaps the worker.
unsigned import_generation();
// Pages imported by the most recently completed import.
int last_import_count();

}  // namespace dusk::guide
