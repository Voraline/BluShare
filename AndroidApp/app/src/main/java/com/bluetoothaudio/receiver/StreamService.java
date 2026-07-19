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
import java.util.UUID;

public final class StreamService extends Service {

    private static final String Tag = "StreamService";
    private static final UUID SppUuid = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final String ChannelId = "BluShareChannel";
    private static final int SampleRate = 48000;
    private static final int Channels = 1;

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
        WorkerThread = new Thread(() -> {
            while (Running) {
                boolean Connected = RunStream(DeviceAddress);
                if (!Running) break;
                UpdateNotification("Reconnecting...");
                try {
                    Thread.sleep(Connected ? 500 : 2000);
                } catch (InterruptedException Ignored) {
                    break;
                }
            }
            stopSelf();
        });
        WorkerThread.start();
    }

    private boolean RunStream(String DeviceAddress) {
        boolean Connected = false;
        try {
            BluetoothAdapter Adapter = BluetoothAdapter.getDefaultAdapter();
            BluetoothDevice Device = Adapter.getRemoteDevice(DeviceAddress);
            Socket = Device.createRfcommSocketToServiceRecord(SppUuid);
            Adapter.cancelDiscovery();
            Socket.connect();
            Connected = true;

            UpdateNotification("Connected");
            InputStream Input = Socket.getInputStream();

            if (!NativeBridge.NativeInit(SampleRate, Channels)) {
                Log.e(Tag, "Failed to initialize native audio player");
                return Connected;
            }

            byte[] LengthBytes = new byte[2];
            byte[] Packet = new byte[4096];
            while (Running) {
                if (!ReadFully(Input, LengthBytes, 2)) break;
                int PacketLength = (LengthBytes[0] & 0xFF) | ((LengthBytes[1] & 0xFF) << 8);
                if (PacketLength <= 0 || PacketLength > 4000) break;
                if (Packet.length < PacketLength) {
                    Packet = new byte[PacketLength];
                }
                if (!ReadFully(Input, Packet, PacketLength)) break;
                NativeBridge.NativeWrite(Packet, PacketLength);
            }
        } catch (Exception Error) {
            Log.e(Tag, "Stream error", Error);
        } finally {
            NativeBridge.NativeShutdown();
            CloseSocket();
        }
        return Connected;
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
            .setContentTitle("BluShare")
            .setContentText(Text)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .build();
    }

    private void UpdateNotification(String Text) {
        NotificationManager Manager = getSystemService(NotificationManager.class);
        Manager.notify(1, BuildNotification(Text));
    }
}