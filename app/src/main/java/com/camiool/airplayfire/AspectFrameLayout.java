package com.camiool.airplayfire;

import android.content.Context;
import android.util.AttributeSet;
import android.widget.FrameLayout;

/** Keeps its child (the video SurfaceView) at the stream's aspect ratio, centered and fitted. */
public class AspectFrameLayout extends FrameLayout {
    private float aspect = 16f / 9f;

    public AspectFrameLayout(Context c) {
        super(c);
    }

    public AspectFrameLayout(Context c, AttributeSet a) {
        super(c, a);
    }

    public void setAspect(int w, int h) {
        if (w <= 0 || h <= 0) return;
        float a = (float) w / h;
        if (Math.abs(a - aspect) > 0.001f) {
            aspect = a;
            requestLayout();
        }
    }

    @Override
    protected void onMeasure(int widthSpec, int heightSpec) {
        int maxW = MeasureSpec.getSize(widthSpec);
        int maxH = MeasureSpec.getSize(heightSpec);
        int w = maxW;
        int h = Math.round(w / aspect);
        if (h > maxH) {
            h = maxH;
            w = Math.round(h * aspect);
        }
        super.onMeasure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
    }
}
