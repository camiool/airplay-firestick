package com.camiool.airplayfire;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;

import java.security.SecureRandom;

/** User preferences + the presets offered in the menu. */
public final class Settings {

    public enum Quality {
        // label, width, height, h265
        FLUIDO("Fluido · 1080p H.264 (menor retraso)", 1920, 1080, false),
        NITIDO("Nítido · 1440p H.265", 2560, 1440, true),
        MAXIMO("Máximo · 4K H.265 (más retraso)", 3840, 2160, true);

        public final String label;
        public final int width;
        public final int height;
        public final boolean h265;

        Quality(String label, int width, int height, boolean h265) {
            this.label = label;
            this.width = width;
            this.height = height;
            this.h265 = h265;
        }
    }

    private static final String PREFS = "airplayfire";
    private final SharedPreferences prefs;

    public Settings(Context ctx) {
        prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public Quality quality() {
        try {
            return Quality.valueOf(prefs.getString("quality", Quality.FLUIDO.name()));
        } catch (IllegalArgumentException e) {
            return Quality.FLUIDO;
        }
    }

    public void setQuality(Quality q) {
        prefs.edit().putString("quality", q.name()).apply();
    }

    public int fps() {
        return prefs.getInt("fps", 60);
    }

    public void setFps(int fps) {
        prefs.edit().putInt("fps", fps).apply();
    }

    public boolean showStats() {
        return prefs.getBoolean("stats", false);
    }

    public void setShowStats(boolean v) {
        prefs.edit().putBoolean("stats", v).apply();
    }

    public boolean audio() {
        return prefs.getBoolean("audio", true);
    }

    public void setAudio(boolean v) {
        prefs.edit().putBoolean("audio", v).apply();
    }

    public String name() {
        String def = "Fire TV " + (Build.MODEL != null ? Build.MODEL : "");
        return prefs.getString("name", def.trim());
    }

    public void setName(String name) {
        prefs.edit().putString("name", name).apply();
    }

    /** Stable, locally administered MAC used as the AirPlay device id. */
    public byte[] hwAddr() {
        String s = prefs.getString("hw", null);
        if (s == null || s.length() != 12) {
            byte[] b = new byte[6];
            new SecureRandom().nextBytes(b);
            b[0] = (byte) ((b[0] & 0xFC) | 0x02); // unicast, locally administered
            StringBuilder sb = new StringBuilder();
            for (byte x : b) sb.append(String.format("%02x", x & 0xff));
            s = sb.toString();
            prefs.edit().putString("hw", s).apply();
        }
        byte[] out = new byte[6];
        for (int i = 0; i < 6; i++) {
            out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        }
        return out;
    }
}
