// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Java port of org.yuzu.yuzu_emu.overlay.OverlayVectorBitmap and OverlayBitmapTint
// (Eden Emulator Project). Modifications: Java port for Polar, 2026; Context.getDrawable
// replaces ContextCompat, and the tinted source is not recycled (the bitmap is cached).

package dev.nxnative.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Paint;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.VectorDrawable;

/** Rasterises Eden's vector artwork to a bitmap at the wanted size, with its fixed tint. */
final class EdenOverlayBitmap {
    private EdenOverlayBitmap() {}

    /** Rasterise drawableId so its longest edge is about targetPx, then tint it. */
    static Bitmap render(Context c, int drawableId, int targetPx, int tint) {
        Drawable d = c.getDrawable(drawableId);
        if (!(d instanceof VectorDrawable) || targetPx <= 0) return null;
        VectorDrawable v = (VectorDrawable) d;
        int iw = Math.max(1, v.getIntrinsicWidth()), ih = Math.max(1, v.getIntrinsicHeight());
        float s = (float) targetPx / Math.max(iw, ih);
        int w = Math.max(1, Math.round(iw * s)), h = Math.max(1, Math.round(ih * s));
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(b);
        v.setBounds(0, 0, w, h);
        v.draw(canvas);
        return tint(b, tint);
    }

    /** The source artwork is left intact: the caller caches it for every control of that kind */
    static Bitmap tint(Bitmap source, int tint) {
        int opaque = tint | 0xFF000000;
        float red = Color.red(opaque) / 255f, green = Color.green(opaque) / 255f, blue = Color.blue(opaque) / 255f;
        if (red >= 0.999f && green >= 0.999f && blue >= 0.999f) return source;
        Bitmap out = Bitmap.createBitmap(source.getWidth(), source.getHeight(), Bitmap.Config.ARGB_8888);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        p.setColorFilter(new ColorMatrixColorFilter(new ColorMatrix(new float[]{
                red, 0, 0, 0, 0,
                0, green, 0, 0, 0,
                0, 0, blue, 0, 0,
                0, 0, 0, 1, 0})));
        new Canvas(out).drawBitmap(source, 0, 0, p);
        return out;
    }
}
