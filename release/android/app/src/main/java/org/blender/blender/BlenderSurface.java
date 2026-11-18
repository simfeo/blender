package org.blender.blender;

import android.content.Context;
import android.graphics.Matrix;
import android.view.MotionEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * SDL surface that can render at a fraction of the view size, for devices that cannot afford
 * full-resolution render targets. The compositor scales the smaller buffers up to the view, so
 * every window-sized GPU buffer shrinks by the square of the divisor.
 *
 * SDL takes its window size from the surface buffers but reads input in view pixels, so input
 * events are scaled into buffer space before SDL sees them.
 */
public class BlenderSurface extends SDLSurface {
    private final int divisor;
    private final Matrix inputScale = new Matrix();

    public BlenderSurface(Context context, int divisor) {
        super(context);
        this.divisor = Math.max(1, divisor);
        inputScale.setScale(1.0f / this.divisor, 1.0f / this.divisor);

        final View.OnGenericMotionListener sdlListener = SDLActivity.getMotionListener();
        setOnGenericMotionListener((v, event) -> {
            MotionEvent scaled = scaledEvent(event);
            try {
                return sdlListener.onGenericMotion(v, scaled);
            }
            finally {
                recycleScaled(scaled, event);
            }
        });
    }

    /* SurfaceView measures itself to the fixed buffer size, which would shrink the view and in turn
     * the buffers again on every layout pass. Always fill the space the parent offers instead. */
    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        if (divisor <= 1) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec);
            return;
        }
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec),
                             MeasureSpec.getSize(heightMeasureSpec));
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        if (divisor > 1 && w > 0 && h > 0) {
            getHolder().setFixedSize(Math.max(1, w / divisor), Math.max(1, h / divisor));
        }
    }

    @Override
    public boolean onTouch(View v, MotionEvent event) {
        MotionEvent scaled = scaledEvent(event);
        try {
            return super.onTouch(v, scaled);
        }
        finally {
            recycleScaled(scaled, event);
        }
    }

    @Override
    public boolean onCapturedPointerEvent(MotionEvent event) {
        MotionEvent scaled = scaledEvent(event);
        try {
            return super.onCapturedPointerEvent(scaled);
        }
        finally {
            recycleScaled(scaled, event);
        }
    }

    private MotionEvent scaledEvent(MotionEvent event) {
        if (divisor <= 1) {
            return event;
        }
        MotionEvent copy = MotionEvent.obtain(event);
        copy.transform(inputScale);
        return copy;
    }

    private static void recycleScaled(MotionEvent scaled, MotionEvent original) {
        if (scaled != original) {
            scaled.recycle();
        }
    }
}
