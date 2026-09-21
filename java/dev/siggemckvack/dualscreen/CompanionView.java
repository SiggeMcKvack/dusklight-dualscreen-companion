package dev.siggemckvack.dualscreen;

import android.content.Context;
import android.graphics.PixelFormat;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * The companion panel: one SurfaceView whose native Surface is handed to the mod via JNI so
 * it can present a WebGPU swapchain into it. Deliberately not routed through SDL's own
 * surface/event pipeline (SDL on Android supports exactly one window).
 *
 * Natives are bound by the mod with RegisterNatives (the mod library exports no Java_* symbols).
 */
final class CompanionView extends SurfaceView {
    static final int TOUCH_DOWN = 0;
    static final int TOUCH_MOVE = 1;
    static final int TOUCH_UP = 2;
    static final int TOUCH_CANCEL = 3;

    private final ScaleGestureDetector mScaleDetector;

    CompanionView(Context context, int bufferWidth, int bufferHeight) {
        super(context);
        // The buffer's shape decides the companion's shape (the dashboard lays itself out
        // from the surface aspect); by default it follows the display. Explicit format so the
        // native side always sees RGBA8888.
        getHolder().setFormat(PixelFormat.RGBA_8888);
        if (bufferWidth > 0 && bufferHeight > 0) {
            getHolder().setFixedSize(bufferWidth, bufferHeight);
        }
        getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                // surfaceChanged always follows with the real dimensions.
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                surfaceChangedGuarded(holder.getSurface(), width, height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                surfaceChangedGuarded(null, 0, 0);
            }
        });
        // Pinch (two fingers) zooms the companion map; while a pinch is active the
        // single-finger stream is cancelled so it cannot register taps or drags.
        mScaleDetector = new ScaleGestureDetector(context,
                new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                    @Override
                    public boolean onScale(ScaleGestureDetector detector) {
                        pinchGuarded(detector.getScaleFactor());
                        return true;
                    }
                });
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        mScaleDetector.onTouchEvent(event);
        if (event.getPointerCount() > 1 || mScaleDetector.isInProgress()) {
            touchGuarded(TOUCH_CANCEL, 0.0f, 0.0f);
            return true;
        }
        final int action = event.getActionMasked();
        if (getWidth() <= 0 || getHeight() <= 0) {
            return false;
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
                touchGuarded(TOUCH_DOWN, event.getX() / getWidth(), event.getY() / getHeight());
                return true;
            case MotionEvent.ACTION_MOVE:
                touchGuarded(TOUCH_MOVE, event.getX() / getWidth(), event.getY() / getHeight());
                return true;
            case MotionEvent.ACTION_UP:
                touchGuarded(TOUCH_UP, event.getX() / getWidth(), event.getY() / getHeight());
                performClick();
                return true;
            case MotionEvent.ACTION_CANCEL:
                // The system took the gesture (e.g. a window change): drop it without a tap, so
                // nothing under the finger activates.
                touchGuarded(TOUCH_CANCEL, 0.0f, 0.0f);
                return true;
            default:
                return false;
        }
    }

    @Override
    public boolean performClick() {
        return super.performClick();
    }

    // The mod unbinds these natives on shutdown, possibly while this view's teardown is still
    // queued on the main thread (the mod waits for it, bounded). A call after that throws
    // UnsatisfiedLinkError; swallow it so a late lifecycle event cannot crash the game.
    private static void surfaceChangedGuarded(Surface surface, int width, int height) {
        try {
            nativeSurfaceChanged(surface, width, height);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private static void touchGuarded(int action, float u, float v) {
        try {
            nativeTouch(action, u, v);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private static void pinchGuarded(float factor) {
        try {
            nativePinch(factor);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    // Normalised u/v in [0,1] over the panel; action is one of TOUCH_*.
    private static native void nativeSurfaceChanged(Surface surface, int width, int height);
    private static native void nativeTouch(int action, float u, float v);
    private static native void nativePinch(float factor);
}
