package com.bluetoothaudio.receiver;

public final class NativeBridge {

    static {
        System.loadLibrary("BluShareNative");
    }

    public static native boolean NativeInit(int SampleRate, int Channels);

    public static native void NativeWrite(byte[] Data, int Length);

    public static native void NativeShutdown();

    private NativeBridge() {
    }
}