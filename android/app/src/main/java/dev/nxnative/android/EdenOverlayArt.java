// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Java port of org.yuzu.yuzu_emu.overlay.OverlayArtwork (Eden Emulator Project).
// Modifications: Java port for Polar, 2026; only the control ids and artwork used by the on-screen pad.

package dev.nxnative.android;

/** Maps Eden's overlay control ids to the vector artwork copied from Eden into res/drawable. */
final class EdenOverlayArt {
    private EdenOverlayArt() {}

    static final String A = "button_a", B = "button_b", X = "button_x", Y = "button_y",
            PLUS = "button_plus", MINUS = "button_minus", HOME = "button_home", CAPTURE = "button_capture",
            L = "button_l", R = "button_r", ZL = "button_zl", ZR = "button_zr",
            STICK_L = "button_stick_l", STICK_R = "button_stick_r",
            LEFT_STICK = "stick_l", RIGHT_STICK = "stick_r", DPAD = "combined_dpad";

    /** The drawable for a control id, 0 when Eden has no artwork for it (SL/SR, Home/Capture) */
    static int drawableFor(String id) {
        if (A.equals(id)) return dev.nxnative.android.R.drawable.facebutton_a;
        if (B.equals(id)) return dev.nxnative.android.R.drawable.facebutton_b;
        if (X.equals(id)) return dev.nxnative.android.R.drawable.facebutton_x;
        if (Y.equals(id)) return dev.nxnative.android.R.drawable.facebutton_y;
        if (PLUS.equals(id)) return dev.nxnative.android.R.drawable.facebutton_plus;
        if (MINUS.equals(id)) return dev.nxnative.android.R.drawable.facebutton_minus;
        if (HOME.equals(id)) return dev.nxnative.android.R.drawable.facebutton_home;
        if (CAPTURE.equals(id)) return dev.nxnative.android.R.drawable.facebutton_screenshot;
        if (L.equals(id)) return dev.nxnative.android.R.drawable.l_shoulder;
        if (R.equals(id)) return dev.nxnative.android.R.drawable.r_shoulder;
        if (ZL.equals(id)) return dev.nxnative.android.R.drawable.zl_trigger;
        if (ZR.equals(id)) return dev.nxnative.android.R.drawable.zr_trigger;
        if (STICK_L.equals(id)) return dev.nxnative.android.R.drawable.button_l3;
        if (STICK_R.equals(id)) return dev.nxnative.android.R.drawable.button_r3;
        if (LEFT_STICK.equals(id) || RIGHT_STICK.equals(id)) return dev.nxnative.android.R.drawable.joystick;
        if (DPAD.equals(id)) return dev.nxnative.android.R.drawable.dpad_standard;
        return 0;
    }

    /** The pressed variant, or the same id when Eden has one only */
    static int pressedFor(String id) {
        if (A.equals(id)) return dev.nxnative.android.R.drawable.facebutton_a_depressed;
        if (B.equals(id)) return dev.nxnative.android.R.drawable.facebutton_b_depressed;
        if (X.equals(id)) return dev.nxnative.android.R.drawable.facebutton_x_depressed;
        if (Y.equals(id)) return dev.nxnative.android.R.drawable.facebutton_y_depressed;
        if (PLUS.equals(id)) return dev.nxnative.android.R.drawable.facebutton_plus_depressed;
        if (MINUS.equals(id)) return dev.nxnative.android.R.drawable.facebutton_minus_depressed;
        if (HOME.equals(id)) return dev.nxnative.android.R.drawable.facebutton_home_depressed;
        if (CAPTURE.equals(id)) return dev.nxnative.android.R.drawable.facebutton_screenshot_depressed;
        if (L.equals(id)) return dev.nxnative.android.R.drawable.l_shoulder_depressed;
        if (R.equals(id)) return dev.nxnative.android.R.drawable.r_shoulder_depressed;
        if (ZL.equals(id)) return dev.nxnative.android.R.drawable.zl_trigger_depressed;
        if (ZR.equals(id)) return dev.nxnative.android.R.drawable.zr_trigger_depressed;
        if (STICK_L.equals(id)) return dev.nxnative.android.R.drawable.button_l3_depressed;
        if (STICK_R.equals(id)) return dev.nxnative.android.R.drawable.button_r3_depressed;
        if (LEFT_STICK.equals(id) || RIGHT_STICK.equals(id)) return dev.nxnative.android.R.drawable.joystick_depressed;
        if (DPAD.equals(id)) return dev.nxnative.android.R.drawable.dpad_standard_cardinal_depressed;
        return 0;
    }

    /** Eden's layout scale for a drawable: how much of the screen's short edge the control covers */
    static float scaleFor(int drawableId) {
        if (drawableId == dev.nxnative.android.R.drawable.facebutton_home || drawableId == dev.nxnative.android.R.drawable.facebutton_screenshot
                || drawableId == dev.nxnative.android.R.drawable.facebutton_plus || drawableId == dev.nxnative.android.R.drawable.facebutton_minus
                || drawableId == dev.nxnative.android.R.drawable.facebutton_plus_depressed || drawableId == dev.nxnative.android.R.drawable.facebutton_minus_depressed)
            return 0.07f;
        if (drawableId == dev.nxnative.android.R.drawable.l_shoulder || drawableId == dev.nxnative.android.R.drawable.r_shoulder
                || drawableId == dev.nxnative.android.R.drawable.zl_trigger || drawableId == dev.nxnative.android.R.drawable.zr_trigger
                || drawableId == dev.nxnative.android.R.drawable.l_shoulder_depressed || drawableId == dev.nxnative.android.R.drawable.r_shoulder_depressed
                || drawableId == dev.nxnative.android.R.drawable.zl_trigger_depressed || drawableId == dev.nxnative.android.R.drawable.zr_trigger_depressed)
            return 0.26f;
        if (drawableId == dev.nxnative.android.R.drawable.button_l3 || drawableId == dev.nxnative.android.R.drawable.button_r3
                || drawableId == dev.nxnative.android.R.drawable.button_l3_depressed || drawableId == dev.nxnative.android.R.drawable.button_r3_depressed)
            return 0.155f;
        if (drawableId == dev.nxnative.android.R.drawable.dpad_standard || drawableId == dev.nxnative.android.R.drawable.dpad_standard_cardinal_depressed
                || drawableId == dev.nxnative.android.R.drawable.dpad_standard_diagonal_depressed) return 0.25f;
        if (drawableId == dev.nxnative.android.R.drawable.joystick || drawableId == dev.nxnative.android.R.drawable.joystick_depressed
                || drawableId == dev.nxnative.android.R.drawable.joystick_range) return 0.3f;
        return 0.11f;
    }
}
