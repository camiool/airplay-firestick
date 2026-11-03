package com.camiool.airplayfire;

import android.view.Surface;

/** Thin JNI wrapper around the native AirPlay receiver (UxPlay core). */
public final class AirPlayNative {
    static {
        System.loadLibrary("airplayfire");
    }

    /** Callbacks invoked from native threads. Implementations must not block. */
    public interface Listener {
        /** Publish an mDNS service; returns a handle for {@link #unregisterService}. */
        int registerService(String name, String type, int port, String[] keys, String[] values);

        void unregisterService(int handle);

        void onClientRequest(String deviceName, String model);

        void onConnectionsChanged(int open);

        void onMirroringChanged(boolean active);

        void onVideoSize(int width, int height);
    }

    private AirPlayNative() {}

    public static native boolean nativeStart(Listener listener, String name, byte[] hwAddr,
                                             int width, int height, int fps, boolean h265,
                                             String keyFile, boolean audio);

    public static native void nativeStop();

    public static native void nativeSetSurface(Surface surface);

    /**
     * [framesIn, rendered, dropped, avgDecodeUs, width, height, h265, lowLatency,
     *  audioType, audioBufferMs, audioXruns, connections]
     */
    public static native long[] nativeGetStats();
}
