package dev.siggemckvack.dualscreen;

import android.app.Activity;
import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.display.DisplayManager;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Keeps the companion Presentation on whichever physical display the game is not using, across
 * display attach/detach and the game activity's pause/resume. Everything runs on the main thread;
 * the mod constructs it on the game thread and starts it with {@link Activity#runOnUiThread}.
 *
 * Lifecycle: {@code new CompanionManager(activity, w, h)} -> {@code activity.runOnUiThread(mgr)}
 * (= start) -> ... -> {@link #stopAndWait()} from the mod's shutdown.
 */
public final class CompanionManager
        implements Runnable, DisplayManager.DisplayListener, Application.ActivityLifecycleCallbacks {
    private static final String TAG = "DualScreenCompanion";

    private final Activity mActivity;
    private final Context mContext;
    private final DisplayManager mDisplayManager;
    private final Handler mMainHandler = new Handler(Looper.getMainLooper());
    private final int mBufferWidth;
    private final int mBufferHeight;
    private final int mGameDisplayId;

    private CompanionPresentation mPresentation;
    private BroadcastReceiver mBatteryReceiver;
    private boolean mStarted;
    private boolean mActivityResumed = true;

    public CompanionManager(Activity activity, int bufferWidth, int bufferHeight) {
        mActivity = activity;
        mContext = activity.getApplicationContext();
        mDisplayManager = (DisplayManager) mContext.getSystemService(Context.DISPLAY_SERVICE);
        mBufferWidth = bufferWidth;
        mBufferHeight = bufferHeight;
        mGameDisplayId = gameDisplayId(activity);
    }

    // The display this activity is actually shown on -- not necessarily DEFAULT_DISPLAY on
    // every dual-screen handheld.
    private static int gameDisplayId(Activity activity) {
        Display display = null;
        try {
            display = activity.getDisplay();
        } catch (RuntimeException ignored) {
        }
        if (display == null && activity.getWindowManager() != null) {
            display = activity.getWindowManager().getDefaultDisplay();
        }
        return display != null ? display.getDisplayId() : Display.DEFAULT_DISPLAY;
    }

    /** Runnable entry so native can start us with Activity.runOnUiThread without another class. */
    @Override
    public void run() {
        start();
    }

    private void start() {
        if (mStarted) {
            return;
        }
        mStarted = true;
        mActivity.getApplication().registerActivityLifecycleCallbacks(this);
        initBatteryMonitor();
        if (mDisplayManager != null) {
            mDisplayManager.registerDisplayListener(this, null);
        }
        showPresentation();
    }

    private void stop() {
        if (!mStarted) {
            return;
        }
        mStarted = false;
        if (mDisplayManager != null) {
            mDisplayManager.unregisterDisplayListener(this);
        }
        if (mBatteryReceiver != null) {
            try {
                mContext.unregisterReceiver(mBatteryReceiver);
            } catch (IllegalArgumentException ignored) {
            }
            mBatteryReceiver = null;
        }
        mActivity.getApplication().unregisterActivityLifecycleCallbacks(this);
        dismissPresentation();
    }

    /**
     * Tear down from any thread and block until the main thread has dismissed the window
     * (so the native surface callback has fired before the mod library unloads).
     */
    public boolean stopAndWait() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            stop();
            return true;
        }
        final CountDownLatch done = new CountDownLatch(1);
        mMainHandler.post(() -> {
            try {
                stop();
            } finally {
                done.countDown();
            }
        });
        try {
            return done.await(2, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    // ---- display selection ----

    // DISPLAY_CATEGORY_PRESENTATION lists the displays a Presentation may attach to (never the
    // default one). Prefer the lowest id that is not the game's: built-in panels enumerate
    // before virtual/overlay ones.
    private Display pickCompanionDisplay() {
        if (mDisplayManager == null) {
            return null;
        }
        Display best = null;
        for (Display display : mDisplayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)) {
            if (display == null || display.getDisplayId() == mGameDisplayId) {
                continue;
            }
            if ((display.getFlags() & Display.FLAG_PRESENTATION) == 0) {
                continue;
            }
            Log.i(TAG, "candidate display " + display.getDisplayId() + " '" + display.getName()
                    + "' " + display.getMode().getPhysicalWidth() + "x"
                    + display.getMode().getPhysicalHeight());
            if (best == null || display.getDisplayId() < best.getDisplayId()) {
                best = display;
            }
        }
        return best;
    }

    private void showPresentation() {
        if (!mStarted || !mActivityResumed || mPresentation != null) {
            return;
        }
        final Display target = pickCompanionDisplay();
        if (target == null) {
            reportAvailable();
            return;
        }
        final CompanionPresentation presentation =
                new CompanionPresentation(mContext, target, mBufferWidth, mBufferHeight);
        presentation.setOnDismissListener(dialog -> {
            // Posted delivery: our own teardown paths clear mPresentation before this
            // arrives, so a still-matching reference means the system dismissed the window
            // behind our back (display flicker, config churn) -- re-show from a fresh message.
            if (mPresentation != presentation) {
                return;
            }
            mPresentation = null;
            reportAvailable();
            mMainHandler.post(this::showPresentation);
        });
        try {
            presentation.show();
            mPresentation = presentation;
            Log.i(TAG, "companion shown on display " + target.getDisplayId());
        } catch (RuntimeException e) {
            // show() fails if the display is claimed elsewhere (e.g. the vendor's own bottom
            // screen launcher) -- degrade to single-screen rather than crash the game.
            Log.w(TAG, "failed to show companion presentation", e);
        }
        reportAvailable();
    }

    private void dismissPresentation() {
        if (mPresentation != null) {
            final CompanionPresentation presentation = mPresentation;
            mPresentation = null;  // cleared first: tells the dismiss listener this was us
            presentation.dismiss();
        }
        reportAvailable();
    }

    private void reportAvailable() {
        try {
            nativeDisplayAvailable(mPresentation != null);
        } catch (UnsatisfiedLinkError ignored) {
            // Natives already unbound by the mod's shutdown; see CompanionView.
        }
    }

    // ---- DisplayManager.DisplayListener ----

    @Override
    public void onDisplayAdded(int displayId) {
        showPresentation();
    }

    @Override
    public void onDisplayRemoved(int displayId) {
        if (mPresentation != null && mPresentation.getDisplay().getDisplayId() == displayId) {
            // The display (and its window) is already gone; just drop the reference.
            mPresentation = null;
            reportAvailable();
        }
    }

    @Override
    public void onDisplayChanged(int displayId) {
    }

    // ---- Application.ActivityLifecycleCallbacks (only the game activity matters) ----

    @Override
    public void onActivityResumed(Activity activity) {
        if (activity == mActivity) {
            mActivityResumed = true;
            showPresentation();
        }
    }

    @Override
    public void onActivityPaused(Activity activity) {
        // A Presentation must not outlive a paused activity (window leak on some devices).
        if (activity == mActivity) {
            mActivityResumed = false;
            dismissPresentation();
        }
    }

    @Override public void onActivityCreated(Activity activity, Bundle savedInstanceState) {}
    @Override public void onActivityStarted(Activity activity) {}
    @Override public void onActivityStopped(Activity activity) {}
    @Override public void onActivitySaveInstanceState(Activity activity, Bundle outState) {}
    @Override public void onActivityDestroyed(Activity activity) {}

    // ---- battery ----

    private void initBatteryMonitor() {
        mBatteryReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                final int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
                final int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                final int status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
                final boolean charging = status == BatteryManager.BATTERY_STATUS_CHARGING
                        || status == BatteryManager.BATTERY_STATUS_FULL;
                if (level >= 0 && scale > 0) {
                    try {
                        nativeBatteryStatus(level * 100 / scale, charging);
                    } catch (UnsatisfiedLinkError ignored) {
                    }
                }
            }
        };
        // ACTION_BATTERY_CHANGED is sticky: registering delivers the current state at once.
        mContext.registerReceiver(mBatteryReceiver, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
    }

    private static native void nativeDisplayAvailable(boolean available);
    private static native void nativeBatteryStatus(int percent, boolean charging);
}
