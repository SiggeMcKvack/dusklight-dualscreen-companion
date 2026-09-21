package dev.siggemckvack.dualscreen;

import android.app.Presentation;
import android.content.Context;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;

/** Hosts {@link CompanionView} on the secondary (bottom) display. */
final class CompanionPresentation extends Presentation {
    private final int mBufferWidth;
    private final int mBufferHeight;

    CompanionPresentation(Context outerContext, Display display, int bufferWidth, int bufferHeight) {
        super(outerContext, display);
        mBufferWidth = bufferWidth;
        mBufferHeight = bufferHeight;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final Window window = getWindow();
        if (window != null) {
            // The second screen gets no wake-resetting input during normal play; keep it
            // from sleeping (and tearing down our surface).
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            // Never take input focus: a focusable Presentation captures the controller after
            // a touch on the bottom screen and gamepad B (KEYCODE_BACK fallback) would dismiss
            // it. Touches still arrive without focus.
            window.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
            applyImmersive(window);
            // Hiding the bars stops them being drawn, but the content frame is still inset
            // for them; consume the insets so the surface is edge-to-edge.
            final View decor = window.getDecorView();
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                decor.setOnApplyWindowInsetsListener((v, insets) -> WindowInsets.CONSUMED);
            } else {
                decor.setOnApplyWindowInsetsListener((v, insets) -> insets.consumeSystemWindowInsets());
            }
            decor.requestApplyInsets();
        }
        // Belt to FLAG_NOT_FOCUSABLE's suspenders: refuse to be cancelled by BACK.
        setCancelable(false);
        setContentView(new CompanionView(getContext(), mBufferWidth, mBufferHeight),
                new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));
    }

    @SuppressWarnings("deprecation")
    static void applyImmersive(Window window) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            // Through the decor view: Window.getInsetsController() dereferences a decor that
            // does not exist yet inside Dialog.onCreate.
            final WindowInsetsController controller =
                    window.getDecorView().getWindowInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }
}
