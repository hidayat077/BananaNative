// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Java port of InputOverlayDrawableButton / InputOverlayDrawableDpad / InputOverlayDrawableJoystick
// (Eden Emulator Project). Modifications: Java port for Polar, 2026. Eden's MotionEvent tracking,
// grid snapping and JNI button events are dropped: the host owns touch arbitration
// (nxin_touch_event) and publishes the pressed/stick state that setPressed/setStick draw here.

package dev.nxnative.android;

import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.drawable.BitmapDrawable;

/** Eden's artwork drawables, driven by the host's on-screen-control state. */
final class EdenOverlayDrawables {
    private EdenOverlayDrawables() {}

    /** A face button, a shoulder or a stick click. */
    static final class Button {
        private final BitmapDrawable normal, pressed;
        private boolean down;
        final int width, height;

        Button(Resources res, Bitmap normalBitmap, Bitmap pressedBitmap) {
            normal = new BitmapDrawable(res, normalBitmap);
            pressed = new BitmapDrawable(res, pressedBitmap);
            width = normal.getIntrinsicWidth();
            height = normal.getIntrinsicHeight();
        }

        void setBounds(int l, int t, int r, int b) {
            normal.setBounds(l, t, r, b);
            pressed.setBounds(l, t, r, b);
        }

        void setOpacity(int a) { normal.setAlpha(a); pressed.setAlpha(a); }

        void setPressed(boolean on) { down = on; }

        void draw(Canvas c) { (down ? pressed : normal).draw(c); }
    }

    /** The 8-state D-pad: Eden rotates one directional bitmap for each pressed direction. */
    static final class Dpad {
        static final int UP = 1, RIGHT = 2, DOWN = 4, LEFT = 8;
        private static final float DEADZONE = 0.5f;
        private final BitmapDrawable base, one, two;
        private final int cxOffset = 0;   // kept for clarity: bounds' centre is the pivot
        private int dir;
        final int width, height;

        Dpad(Resources res, Bitmap baseBitmap, Bitmap cardinal, Bitmap diagonal) {
            base = new BitmapDrawable(res, baseBitmap);
            one = new BitmapDrawable(res, cardinal);
            two = new BitmapDrawable(res, diagonal);
            width = base.getIntrinsicWidth();
            height = base.getIntrinsicHeight();
        }

        void setBounds(int l, int t, int r, int b) {
            base.setBounds(l, t, r, b);
            one.setBounds(l, t, r, b);
            two.setBounds(l, t, r, b);
        }

        void setOpacity(int a) { base.setAlpha(a); one.setAlpha(a); two.setAlpha(a); }

        /** A bit mask of UP / RIGHT / DOWN / LEFT, as the host publishes it. */
        void setDirections(int mask) { dir = mask; }

        void draw(Canvas c) {
            boolean up = (dir & UP) != 0, right = (dir & RIGHT) != 0, down = (dir & DOWN) != 0, left = (dir & LEFT) != 0;
            Rect r = base.getBounds();
            float px = r.centerX() + cxOffset, py = r.centerY();
            if (up && !left && !right) { one.draw(c); return; }
            if (down && !left && !right) { rotate(c, 180, px, py, one); return; }
            if (left && !up && !down) { rotate(c, 270, px, py, one); return; }
            if (right && !up && !down) { rotate(c, 90, px, py, one); return; }
            if (up && left && !right) { two.draw(c); return; }
            if (up && right && !down) { rotate(c, 90, px, py, two); return; }
            if (down && right && !left) { rotate(c, 180, px, py, two); return; }
            if (down && left && !right) { rotate(c, 270, px, py, two); return; }
            base.draw(c);
        }

        private static void rotate(Canvas c, float deg, float x, float y, BitmapDrawable d) {
            c.save(); c.rotate(deg, x, y); d.draw(c); c.restore();
        }

        /** The direction the pointer is pushed to, or 0 inside the dead zone. Eden's VIRT_AXIS_DEADZONE. */
        static int directions(float x, float y) {
            int d = 0;
            if (y < -DEADZONE) d |= UP;
            if (y > DEADZONE) d |= DOWN;
            if (x < -DEADZONE) d |= LEFT;
            if (x > DEADZONE) d |= RIGHT;
            return d;
        }
    }

    /** Outer ring plus the inner cap, clamped to a circle like Eden's joystick. */
    static final class Joystick {
        private final BitmapDrawable outer, inner, innerPressed;
        private final Rect bounds;
        private int opacity = 255;
        private float x, y;
        final int width, height;

        Joystick(Resources res, Bitmap outerBitmap, Bitmap innerBitmap, Bitmap innerPressedBitmap, Rect outerRect, Rect innerRect) {
            outer = new BitmapDrawable(res, outerBitmap);
            inner = new BitmapDrawable(res, innerBitmap);
            innerPressed = new BitmapDrawable(res, innerPressedBitmap);
            width = outerBitmap.getWidth();
            height = outerBitmap.getHeight();
            bounds = new Rect(outerRect);
            outer.setBounds(bounds);
            Rect cap = new Rect(innerRect);
            if (cap.width() <= 0 || cap.height() <= 0) {
                int hw = Math.max(1, innerBitmap.getWidth() / 2), hh = Math.max(1, innerBitmap.getHeight() / 2);
                cap.set(bounds.centerX() - hw, bounds.centerY() - hh, bounds.centerX() + hw, bounds.centerY() + hh);
            }
            inner.setBounds(cap);
            innerPressed.setBounds(cap);
        }

        void setBounds(int l, int t, int r, int b) {
            bounds.set(l, t, r, b);
            outer.setBounds(bounds);
            positionInner();
        }

        void setOpacity(int a) {
            opacity = a;
            outer.setAlpha(a);
            inner.setAlpha(a);
            innerPressed.setAlpha(a);
        }

        /** Host stick values in -1..1; y is already correct for the picture (the host flips it). */
        void setStick(float sx, float sy) {
            x = Math.max(-1, Math.min(1, sx));
            y = Math.max(-1, Math.min(1, sy));
            positionInner();
        }

        private void positionInner() {
            int cx = bounds.centerX() + Math.round(x * bounds.width() / 2);
            int cy = bounds.centerY() + Math.round(y * bounds.height() / 2);
            int hw = inner.getBounds().width() / 2, hh = inner.getBounds().height() / 2;
            if (hw == 0 || hh == 0) return;
            inner.setBounds(cx - hw, cy - hh, cx + hw, cy + hh);
            innerPressed.setBounds(inner.getBounds());
        }

        void draw(Canvas c) {
            outer.setAlpha(opacity);
            outer.draw(c);
            inner.draw(c);
        }
    }
}
