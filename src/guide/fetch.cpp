#include "dusk/guide/fetch.hpp"

#include <atomic>
#include <memory>
#include <thread>

#include <fmt/format.h>

#include "dusk/guide/store.hpp"

namespace dusk::guide {

// Internal: the import worker's image supplier.
ImageSource network_image_source();

namespace {

// One scan_import_folder() run on a worker thread; joined when reaped.
class ImportTask {
public:
    ImportTask() {
        mWorker = std::thread([this] {
            int n = 0;
            try {
                n = scan_import_folder(network_image_source());
            } catch (...) {
                n = 0;
            }
            mCount = n;
            mDone.store(true, std::memory_order_release);
        });
    }
    ~ImportTask() {
        if (mWorker.joinable()) {
            mWorker.join();
        }
    }
    ImportTask(const ImportTask&) = delete;
    ImportTask& operator=(const ImportTask&) = delete;
    bool finished() const { return mDone.load(std::memory_order_acquire); }
    int count() const { return mCount; }

private:
    std::thread mWorker;
    std::atomic<bool> mDone{false};
    int mCount = 0;
};

std::unique_ptr<ImportTask> s_import;
unsigned s_importGen = 0;
int s_lastImportCount = 0;

// Joins and clears a finished worker, exactly once, whichever consumer asks
// first. Game thread only.
void reap_import() {
    if (s_import != nullptr && s_import->finished()) {
        s_lastImportCount = s_import->count();
        s_import.reset();  // joins
        s_importGen++;
    }
}

}  // namespace

void begin_import() {
    if (s_import == nullptr) {
        s_import = std::make_unique<ImportTask>();
    }
}

bool import_in_progress() {
    reap_import();
    return s_import != nullptr;
}

unsigned import_generation() {
    reap_import();
    return s_importGen;
}

int last_import_count() {
    return s_lastImportCount;
}

ImageSource network_image_source() {
    // The mod HttpService is asynchronous and game-thread only, and this runs on the import
    // worker. The in-app browser hands image bytes over when it saves a page, so nothing is
    // fetched here; images a manually saved page did not bring along are skipped.
    return {};
}

}  // namespace dusk::guide
