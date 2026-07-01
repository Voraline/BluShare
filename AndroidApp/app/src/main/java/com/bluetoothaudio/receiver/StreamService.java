package com.bluetoothaudio.receiver;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothSocket;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.UUID;

public final class StreamService extends Service {

    private static final String TAG = "StreamService";
    private static final UUID SppUuid = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final int StreamMagic = 0x42415354;
    private static final String ChannelId = "BluetoothAudioChannel";

    private Thread WorkerThread;
    private volatile boolean Running = false;
    private BluetoothSocket Socket;

    @Override
    public void onCreate() {
        super.onCreate();
        CreateNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent Intent, int Flags, int StartId) {
        String DeviceAddress = Intent != null ? Intent.getStringExtra("DeviceAddress") : null;
        startForeground(1, BuildNotification("Connecting..."));

        if (DeviceAddress == null) {
            stopSelf();
            return START_NOT_STICKY;
        }

        StartWorker(DeviceAddress);
        return START_STICKY;
    }

    private void StartWorker(String DeviceAddress) {
        Running = true;
        WorkerThread = new Thread(() -> RunStream(DeviceAddress));
        WorkerThread.start();
    }

    private void RunStream(String DeviceAddress) {
        try {
            BluetoothAdapter Adapter = BluetoothAdapter.getDefaultAdapter();
            BluetoothDevice Device = Adapter.getRemoteDevice(DeviceAddress);
            Socket = Device.createRfcommSocketToServiceRecord(SppUuid);
            Adapter.cancelDiscovery();
            Socket.connect();

            UpdateNotification("Connected");
            InputStream Input = Socket.getInputStream();

            byte[] HeaderBytes = new byte[12];
            if (!ReadFully(Input, HeaderBytes, 12)) return;

            ByteBuffer HeaderBuffer = ByteBuffer.wrap(HeaderBytes).order(ByteOrder.LITTLE_ENDIAN);
            int Magic = HeaderBuffer.getInt();
            int SampleRate = HeaderBuffer.getInt();
            short Channels = HeaderBuffer.getShort();
            short BitsPerSample = HeaderBuffer.getShort();

            if (Magic != StreamMagic) {
                Log.e(TAG, "Invalid stream header");
                return;
            }

            if (!NativeBridge.NativeInit(SampleRate, Channels, BitsPerSample)) {
                Log.e(TAG, "Failed to initialize native audio player");
                return;
            }

            byte[] Buffer = new byte[8192];
            while (Running) {
                int Read = Input.read(Buffer);
                if (Read <= 0) break;
                NativeBridge.NativeWrite(Buffer, Read);
            }
        } catch (Exception Error) {
            Log.e(TAG, "Stream error", Error);
        } finally {
            NativeBridge.NativeShutdown();
            CloseSocket();
            stopSelf();
        }
    }

    private boolean ReadFully(InputStream Input, byte[] Buffer, int Length) throws Exception {
        int Offset = 0;
        while (Offset < Length) {
            int Read = Input.read(Buffer, Offset, Length - Offset);
            if (Read <= 0) return false;
            Offset += Read;
        }
        return true;
    }

    private void CloseSocket() {
        try {
            if (Socket != null) Socket.close();
        } catch (Exception Ignored) {
        }
    }

    @Override
    public void onDestroy() {
        Running = false;
        CloseSocket();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent Intent) {
        return null;
    }

    private void CreateNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel Channel = new NotificationChannel(ChannelId, "Bluetooth Audio",
                NotificationManager.IMPORTANCE_LOW);
            NotificationManager Manager = getSystemService(NotificationManager.class);
            Manager.createNotificationChannel(Channel);
        }
    }

    private Notification BuildNotification(String Text) {
        return new Notification.Builder(this, ChannelId)
            .setContentTitle("Bluetooth Audio Receiver")
            .setContentText(Text)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .build();
    }

    private void UpdateNotification(String Text) {
        NotificationManager Manager = getSystemService(NotificationManager.class);
        Manager.notify(1, BuildNotification(Text));
    }
}
