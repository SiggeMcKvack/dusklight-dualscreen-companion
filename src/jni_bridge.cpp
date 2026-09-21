#include "jni_bridge.hpp"
#include "present.hpp"

#include "dusk/guide/fetch.hpp"
#include "dusk/guide/store.hpp"

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include <android/native_window_jni.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

extern const unsigned char g_dualScreenDex[];
extern const size_t g_dualScreenDex_size;

// SDL's JNI entry points, exported by libmain.so. Hooking one of them is how the mod obtains a
// JNIEnv when the symbol manifest cannot hand out SDL_GetAndroidJNIEnv: they run on the Android
// UI thread with the env as their first argument.
extern "C" {
JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_onNativeAccel(
    JNIEnv* env, jclass cls, jfloat x, jfloat y, jfloat z);
JNIEXPORT void JNICALL Java_org_libsdl_app_SDLActivity_onNativeTouch(JNIEnv* env, jclass cls,
    jint touchDeviceId, jint pointerFingerId, jint action, jfloat x, jfloat y, jfloat p);
JNIEXPORT jboolean JNICALL Java_org_libsdl_app_SDLControllerManager_onNativePadDown(
    JNIEnv* env, jclass cls, jint deviceId, jint keycode, jint scancode);
}

DEFINE_HOOK(&Java_org_libsdl_app_SDLActivity_onNativeAccel, JniAccel);
DEFINE_HOOK(&Java_org_libsdl_app_SDLActivity_onNativeTouch, JniTouch);
DEFINE_HOOK(&Java_org_libsdl_app_SDLControllerManager_onNativePadDown, JniPadDown);

namespace dsc::jni {
namespace {

constexpr const char* kPackage = "dev/siggemckvack/dualscreen/";

// ---- state written from the UI thread, read on the game thread ----

std::atomic<JavaVM*> g_vm{nullptr};
std::atomic<bool> g_bootstrapped{false};
std::atomic<bool> g_displayAvailable{false};
std::atomic<int> g_batteryPercent{-1};
std::atomic<bool> g_batteryCharging{false};

std::mutex g_surfaceMutex;
std::condition_variable g_surfaceAck;
bool g_surfaceDirty = false;
uint64_t g_surfaceGen = 0;      // bumped per posted change
uint64_t g_surfaceAckGen = 0;   // last change the game thread applied
SurfaceChange g_pendingSurface{};

constexpr size_t kTouchRing = 256;
std::mutex g_touchMutex;
TouchEvent g_touch[kTouchRing];
size_t g_touchHead = 0;
size_t g_touchCount = 0;
float g_pinch = 1.0f;

// ---- game-thread state ----

uint32_t g_panelWidth = 0;
uint32_t g_panelHeight = 0;
bool g_hooksInstalled = false;
bool g_threadAttachedByUs = false;
jobject g_activity = nullptr;   // global ref
jobject g_loader = nullptr;     // global ref (InMemoryDexClassLoader)
jclass g_viewClass = nullptr;   // global ref
jclass g_managerClass = nullptr;
jclass g_browserClass = nullptr;
jobject g_manager = nullptr;    // global ref

// ---- natives (UI thread; cheap, no services) ----

void JNICALL native_surface_changed(JNIEnv* env, jclass, jobject surface, jint width, jint height) {
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::unique_lock lock{g_surfaceMutex};
    if (g_surfaceDirty && g_pendingSurface.window != nullptr) {
        ANativeWindow_release(g_pendingSurface.window);  // superseded before the game saw it
    }
    g_pendingSurface = SurfaceChange{
        .window = window,
        .width = static_cast<uint32_t>(width > 0 ? width : 0),
        .height = static_cast<uint32_t>(height > 0 ? height : 0),
    };
    g_surfaceDirty = true;
    const uint64_t gen = ++g_surfaceGen;
    if (window == nullptr && dsc::present::target_active()) {
        // Android destroys the Surface as soon as surfaceDestroyed returns. Give the game
        // thread a chance to drop the swapchain first; bounded, since it may be parked (pause)
        // or not ticking the mod at all (pre-launch UI), and in those cases nothing presents.
        g_surfaceAck.wait_for(lock, std::chrono::milliseconds(500),
            [&] { return g_surfaceAckGen >= gen || !dsc::present::target_active(); });
    }
}

void JNICALL native_touch(JNIEnv*, jclass, jint action, jfloat u, jfloat v) {
    std::lock_guard lock{g_touchMutex};
    if (g_touchCount == kTouchRing) {
        return;  // drop on overflow
    }
    g_touch[(g_touchHead + g_touchCount) % kTouchRing] = TouchEvent{action, u, v};
    ++g_touchCount;
}

void JNICALL native_pinch(JNIEnv*, jclass, jfloat factor) {
    std::lock_guard lock{g_touchMutex};
    if (factor > 0.0f) {
        g_pinch *= factor;
    }
}

// ---- guide browser natives (UI thread) ----

jstring JNICALL native_guides_root(JNIEnv* env, jclass) {
    std::string path;
    try {
        path = dusk::guide::guides_root().string();
    } catch (...) {
        path.clear();
    }
    return env->NewStringUTF(path.c_str());
}

void JNICALL native_guides_import(JNIEnv*, jclass) {
    // Kicks the import worker; idempotent while one is running.
    dusk::guide::begin_import();
}

void JNICALL native_display_available(JNIEnv*, jclass, jboolean available) {
    g_displayAvailable.store(available != JNI_FALSE);
}

void JNICALL native_battery_status(JNIEnv*, jclass, jint percent, jboolean charging) {
    g_batteryPercent.store(percent);
    g_batteryCharging.store(charging != JNI_FALSE);
}

// ---- JavaVM capture via SDL's JNI entry points (UI thread) ----

HookAction on_sdl_jni_entry(ModContext*, void* args, void*, void*) {
    if (g_vm.load(std::memory_order_relaxed) == nullptr) {
        JNIEnv* env = mods::arg<JNIEnv*>(args, 0);
        JavaVM* vm = nullptr;
        if (env != nullptr && env->GetJavaVM(&vm) == JNI_OK) {
            g_vm.store(vm);
        }
    }
    return HOOK_CONTINUE;
}

bool install_capture_hooks() {
    // Any one of these firing is enough; they stay installed as no-ops afterwards so the UI
    // thread never races an uninstall's code patch.
    int installed = 0;
    if (mods::hook::add_pre<JniAccel>(on_sdl_jni_entry) == MOD_OK) {
        ++installed;
    }
    if (mods::hook::add_pre<JniTouch>(on_sdl_jni_entry) == MOD_OK) {
        ++installed;
    }
    if (mods::hook::add_pre<JniPadDown>(on_sdl_jni_entry) == MOD_OK) {
        ++installed;
    }
    g_hooksInstalled = installed > 0;
    return g_hooksInstalled;
}

// Direct route: SDL's accessors are in the linked binary; the symbol manifest may expose them.
bool try_direct_capture() {
    void* getEnv = nullptr;
    if (svc_hook->resolve(mod_ctx, "SDL_GetAndroidJNIEnv", &getEnv, nullptr) != MOD_OK ||
        getEnv == nullptr)
    {
        return false;
    }
    auto* env = static_cast<JNIEnv*>(reinterpret_cast<void* (*)()>(getEnv)());
    JavaVM* vm = nullptr;
    if (env == nullptr || env->GetJavaVM(&vm) != JNI_OK) {
        return false;
    }
    g_vm.store(vm);
    mods::log::info("JavaVM obtained through SDL_GetAndroidJNIEnv");
    return true;
}

// ---- bootstrap (game thread) ----

JNIEnv* attach_game_thread() {
    JavaVM* vm = g_vm.load();
    if (vm == nullptr) {
        return nullptr;
    }
    JNIEnv* env = nullptr;
    const jint status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_OK) {
        return env;
    }
    if (status != JNI_EDETACHED) {
        return nullptr;
    }
    JavaVMAttachArgs args{JNI_VERSION_1_6, "DuskGameThread", nullptr};
    if (vm->AttachCurrentThread(&env, &args) != JNI_OK) {
        return nullptr;
    }
    g_threadAttachedByUs = true;
    return env;
}

bool check_exception(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    mods::log::error("JNI exception during {}", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

// FindClass resolves app classes here because the game thread is SDL's Java-created thread
// (SDLActivity.SDLMain -> nativeRunMain -> main -> main01), so its context class loader is the
// app's. A thread attached from native would only see boot classes.
jobject find_activity(JNIEnv* env) {
    jclass sdlActivity = env->FindClass("org/libsdl/app/SDLActivity");
    if (check_exception(env, "FindClass SDLActivity") || sdlActivity == nullptr) {
        return nullptr;
    }
    jmethodID getContext =
        env->GetStaticMethodID(sdlActivity, "getContext", "()Landroid/app/Activity;");
    if (check_exception(env, "SDLActivity.getContext lookup") || getContext == nullptr) {
        env->DeleteLocalRef(sdlActivity);
        return nullptr;
    }
    jobject activity = env->CallStaticObjectMethod(sdlActivity, getContext);
    env->DeleteLocalRef(sdlActivity);
    if (check_exception(env, "SDLActivity.getContext") || activity == nullptr) {
        return nullptr;
    }
    return activity;
}

jobject load_dex(JNIEnv* env, jobject activity) {
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getClassLoader =
        env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    env->DeleteLocalRef(activityClass);
    if (check_exception(env, "getClassLoader lookup")) {
        return nullptr;
    }
    jobject parent = env->CallObjectMethod(activity, getClassLoader);
    if (check_exception(env, "getClassLoader") || parent == nullptr) {
        return nullptr;
    }
    // ART copies the bytes out of the buffer during construction.
    jobject buffer = env->NewDirectByteBuffer(
        const_cast<unsigned char*>(g_dualScreenDex), static_cast<jlong>(g_dualScreenDex_size));
    if (check_exception(env, "NewDirectByteBuffer") || buffer == nullptr) {
        env->DeleteLocalRef(parent);
        return nullptr;
    }
    jclass loaderClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (check_exception(env, "FindClass InMemoryDexClassLoader") || loaderClass == nullptr) {
        env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(parent);
        return nullptr;
    }
    jmethodID ctor =
        env->GetMethodID(loaderClass, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    jobject loader = ctor != nullptr ? env->NewObject(loaderClass, ctor, buffer, parent) : nullptr;
    env->DeleteLocalRef(loaderClass);
    env->DeleteLocalRef(buffer);
    env->DeleteLocalRef(parent);
    if (check_exception(env, "InMemoryDexClassLoader") || loader == nullptr) {
        return nullptr;
    }
    return loader;
}

jclass load_class(JNIEnv* env, jobject loader, const char* simpleName) {
    jclass loaderClass = env->GetObjectClass(loader);
    jmethodID loadClass =
        env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    env->DeleteLocalRef(loaderClass);
    if (check_exception(env, "loadClass lookup")) {
        return nullptr;
    }
    std::string name = std::string(kPackage) + simpleName;
    for (char& c : name) {
        if (c == '/') {
            c = '.';
        }
    }
    jstring jname = env->NewStringUTF(name.c_str());
    jobject cls = env->CallObjectMethod(loader, loadClass, jname);
    env->DeleteLocalRef(jname);
    if (check_exception(env, name.c_str()) || cls == nullptr) {
        return nullptr;
    }
    return static_cast<jclass>(cls);
}

bool bind_natives(JNIEnv* env) {
    const JNINativeMethod viewMethods[] = {
        {"nativeSurfaceChanged", "(Landroid/view/Surface;II)V",
            reinterpret_cast<void*>(native_surface_changed)},
        {"nativeTouch", "(IFF)V", reinterpret_cast<void*>(native_touch)},
        {"nativePinch", "(F)V", reinterpret_cast<void*>(native_pinch)},
    };
    const JNINativeMethod managerMethods[] = {
        {"nativeDisplayAvailable", "(Z)V", reinterpret_cast<void*>(native_display_available)},
        {"nativeBatteryStatus", "(IZ)V", reinterpret_cast<void*>(native_battery_status)},
    };
    if (env->RegisterNatives(g_viewClass, viewMethods, 3) != JNI_OK ||
        check_exception(env, "RegisterNatives CompanionView"))
    {
        return false;
    }
    if (env->RegisterNatives(g_managerClass, managerMethods, 2) != JNI_OK ||
        check_exception(env, "RegisterNatives CompanionManager"))
    {
        return false;
    }
    const JNINativeMethod browserMethods[] = {
        {"nativeGuidesRoot", "()Ljava/lang/String;", reinterpret_cast<void*>(native_guides_root)},
        {"nativeGuidesImport", "()V", reinterpret_cast<void*>(native_guides_import)},
    };
    if (env->RegisterNatives(g_browserClass, browserMethods, 2) != JNI_OK ||
        check_exception(env, "RegisterNatives GuideBrowser"))
    {
        return false;
    }
    return true;
}

bool start_manager(JNIEnv* env) {
    jmethodID ctor = env->GetMethodID(g_managerClass, "<init>", "(Landroid/app/Activity;II)V");
    if (check_exception(env, "CompanionManager ctor lookup") || ctor == nullptr) {
        return false;
    }
    jobject manager = env->NewObject(g_managerClass, ctor, g_activity,
        static_cast<jint>(g_panelWidth), static_cast<jint>(g_panelHeight));
    if (check_exception(env, "CompanionManager ctor") || manager == nullptr) {
        return false;
    }
    g_manager = env->NewGlobalRef(manager);
    env->DeleteLocalRef(manager);

    jclass activityClass = env->GetObjectClass(g_activity);
    jmethodID runOnUiThread =
        env->GetMethodID(activityClass, "runOnUiThread", "(Ljava/lang/Runnable;)V");
    env->DeleteLocalRef(activityClass);
    if (check_exception(env, "runOnUiThread lookup") || runOnUiThread == nullptr) {
        return false;
    }
    env->CallVoidMethod(g_activity, runOnUiThread, g_manager);  // CompanionManager.run == start
    return !check_exception(env, "runOnUiThread");
}

void release_java_refs(JNIEnv* env) {
    if (g_manager != nullptr) {
        env->DeleteGlobalRef(g_manager);
        g_manager = nullptr;
    }
    if (g_viewClass != nullptr) {
        env->DeleteGlobalRef(g_viewClass);
        g_viewClass = nullptr;
    }
    if (g_managerClass != nullptr) {
        env->DeleteGlobalRef(g_managerClass);
        g_managerClass = nullptr;
    }
    if (g_browserClass != nullptr) {
        env->DeleteGlobalRef(g_browserClass);
        g_browserClass = nullptr;
    }
    if (g_loader != nullptr) {
        env->DeleteGlobalRef(g_loader);
        g_loader = nullptr;
    }
    if (g_activity != nullptr) {
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
    }
}

bool bootstrap(JNIEnv* env) {
    jobject activity = find_activity(env);
    if (activity == nullptr) {
        return false;
    }
    g_activity = env->NewGlobalRef(activity);
    env->DeleteLocalRef(activity);

    jobject loader = load_dex(env, g_activity);
    if (loader == nullptr) {
        return false;
    }
    g_loader = env->NewGlobalRef(loader);
    env->DeleteLocalRef(loader);

    jclass viewClass = load_class(env, g_loader, "CompanionView");
    jclass managerClass = load_class(env, g_loader, "CompanionManager");
    jclass browserClass = load_class(env, g_loader, "GuideBrowser");
    if (viewClass == nullptr || managerClass == nullptr || browserClass == nullptr) {
        return false;
    }
    g_viewClass = static_cast<jclass>(env->NewGlobalRef(viewClass));
    g_managerClass = static_cast<jclass>(env->NewGlobalRef(managerClass));
    g_browserClass = static_cast<jclass>(env->NewGlobalRef(browserClass));
    env->DeleteLocalRef(viewClass);
    env->DeleteLocalRef(managerClass);
    env->DeleteLocalRef(browserClass);

    return bind_natives(env) && start_manager(env);
}

}  // namespace

bool init(uint32_t panelWidth, uint32_t panelHeight) {
    g_panelWidth = panelWidth;
    g_panelHeight = panelHeight;
    if (try_direct_capture()) {
        return true;
    }
    if (!install_capture_hooks()) {
        mods::log::error("could not hook any SDL JNI entry point; no JavaVM available");
        return false;
    }
    mods::log::info("waiting for a JavaVM from SDL's JNI entry points");
    return true;
}

void update() {
    if (g_bootstrapped.load(std::memory_order_relaxed) || g_vm.load() == nullptr) {
        return;
    }
    JNIEnv* env = attach_game_thread();
    if (env == nullptr) {
        mods::log::error("failed to attach the game thread to the JavaVM");
        g_bootstrapped.store(true);  // don't retry every frame
        return;
    }
    if (bootstrap(env)) {
        mods::log::info("companion Java side started");
    } else {
        mods::log::error("companion Java bootstrap failed");
        release_java_refs(env);
    }
    g_bootstrapped.store(true);
}

void shutdown() {
    JavaVM* vm = g_vm.load();
    if (vm != nullptr && g_bootstrapped.load()) {
        JNIEnv* env = attach_game_thread();
        if (env != nullptr) {
            if (g_manager != nullptr) {
                jmethodID stopAndWait = env->GetMethodID(g_managerClass, "stopAndWait", "()Z");
                if (!check_exception(env, "stopAndWait lookup") && stopAndWait != nullptr) {
                    if (env->CallBooleanMethod(g_manager, stopAndWait) == JNI_FALSE) {
                        mods::log::warn("companion presentation did not dismiss in time");
                    }
                    check_exception(env, "stopAndWait");
                }
            }
            // If stopAndWait timed out, the queued UI-thread teardown can still call back after
            // this library is gone. Unbinding turns that into an UnsatisfiedLinkError, which the
            // Java side catches and ignores, instead of a jump into unmapped code.
            if (g_viewClass != nullptr) {
                env->UnregisterNatives(g_viewClass);
            }
            if (g_managerClass != nullptr) {
                env->UnregisterNatives(g_managerClass);
            }
            if (g_browserClass != nullptr) {
                env->UnregisterNatives(g_browserClass);
            }
            check_exception(env, "UnregisterNatives");
            release_java_refs(env);
            if (g_threadAttachedByUs) {
                vm->DetachCurrentThread();
                g_threadAttachedByUs = false;
            }
        }
    }
    {
        std::lock_guard lock{g_surfaceMutex};
        if (g_surfaceDirty && g_pendingSurface.window != nullptr) {
            ANativeWindow_release(g_pendingSurface.window);
        }
        g_pendingSurface = SurfaceChange{};
        g_surfaceDirty = false;
    }
    {
        std::lock_guard lock{g_touchMutex};
        g_touchCount = 0;
        g_pinch = 1.0f;
    }
    g_displayAvailable.store(false);
    g_bootstrapped.store(false);
    g_vm.store(nullptr);
}
// (Method ids cached in vibrate() are per-class; the library is reloaded fresh on re-enable,
// which resets the static.)

bool is_bootstrapped() {
    return g_bootstrapped.load() && g_manager != nullptr;
}

void vibrate(uint32_t durationMs, float amplitude) {
    if (!is_bootstrapped() || g_managerClass == nullptr || durationMs == 0) {
        return;
    }
    JNIEnv* env = attach_game_thread();
    if (env == nullptr) {
        return;
    }
    static jmethodID method = nullptr;
    if (method == nullptr) {
        method = env->GetStaticMethodID(
            g_managerClass, "vibrate", "(Landroid/content/Context;II)V");
        if (check_exception(env, "CompanionManager.vibrate lookup") || method == nullptr) {
            return;
        }
    }
    const int amp = static_cast<int>(amplitude * 255.0f + 0.5f);
    env->CallStaticVoidMethod(g_managerClass, method, g_activity, static_cast<jint>(durationMs),
        static_cast<jint>(amp < 1 ? 1 : (amp > 255 ? 255 : amp)));
    check_exception(env, "CompanionManager.vibrate");
}

bool open_guide_browser(const char* url) {
    if (!is_bootstrapped() || g_browserClass == nullptr) {
        return false;
    }
    JNIEnv* env = attach_game_thread();
    if (env == nullptr) {
        return false;
    }
    jmethodID open = env->GetStaticMethodID(
        g_browserClass, "open", "(Landroid/app/Activity;Ljava/lang/String;)V");
    if (check_exception(env, "GuideBrowser.open lookup") || open == nullptr) {
        return false;
    }
    jstring jurl = env->NewStringUTF(url);
    env->CallStaticVoidMethod(g_browserClass, open, g_activity, jurl);
    env->DeleteLocalRef(jurl);
    return !check_exception(env, "GuideBrowser.open");
}

bool take_surface_change(SurfaceChange& out) {
    std::lock_guard lock{g_surfaceMutex};
    if (!g_surfaceDirty) {
        return false;
    }
    out = g_pendingSurface;
    g_pendingSurface = SurfaceChange{};
    g_surfaceDirty = false;
    return true;
}

void ack_surface_change() {
    {
        std::lock_guard lock{g_surfaceMutex};
        g_surfaceAckGen = g_surfaceGen;
    }
    g_surfaceAck.notify_all();
}

size_t drain_touch(TouchEvent* out, size_t max) {
    std::lock_guard lock{g_touchMutex};
    size_t n = 0;
    while (n < max && g_touchCount > 0) {
        out[n++] = g_touch[g_touchHead];
        g_touchHead = (g_touchHead + 1) % kTouchRing;
        --g_touchCount;
    }
    return n;
}

float take_pinch() {
    std::lock_guard lock{g_touchMutex};
    const float factor = g_pinch;
    g_pinch = 1.0f;
    return factor;
}

bool display_available() {
    return g_displayAvailable.load();
}

int battery_percent() {
    return g_batteryPercent.load();
}

bool battery_charging() {
    return g_batteryCharging.load();
}

}  // namespace dsc::jni
