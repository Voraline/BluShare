package com.bluetoothaudio.receiver;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.util.Set;

public final class MainActivity extends Activity {

    private static final int PermissionRequestCode = 1001;
    private static final int ColorBackground = Color.parseColor("#121212");
    private static final int ColorSurface = Color.parseColor("#1E1E1E");
    private static final int ColorAccent = Color.parseColor("#4FC3F7");
    private static final int ColorTextPrimary = Color.parseColor("#FFFFFF");
    private LinearLayout DeviceListLayout;

    @Override
    protected void onCreate(Bundle SavedInstanceState) {
        super.onCreate(SavedInstanceState);
        BuildUi();
        RequestNeededPermissions();
    }

    private void BuildUi() {
        LinearLayout Root = new LinearLayout(this);
        Root.setOrientation(LinearLayout.VERTICAL);
        Root.setPadding(32, 64, 32, 32);
        Root.setBackgroundColor(ColorBackground);

        DeviceListLayout = new LinearLayout(this);
        DeviceListLayout.setOrientation(LinearLayout.VERTICAL);
        Root.addView(DeviceListLayout);

        Button RefreshButton = new Button(this);
        RefreshButton.setText("Refresh Paired Devices");
        RefreshButton.setBackgroundColor(ColorSurface);
        RefreshButton.setTextColor(ColorAccent);
        RefreshButton.setOnClickListener(View -> PopulateDeviceList());
        Root.addView(RefreshButton);

        ScrollView Scroll = new ScrollView(this);
        Scroll.setBackgroundColor(ColorBackground);
        Scroll.addView(Root);
        setContentView(Scroll);
    }

    private void RequestNeededPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            String[] Permissions = {
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_SCAN
            };
            boolean NeedsRequest = false;
            for (String Permission : Permissions) {
                if (ContextCompat.checkSelfPermission(this, Permission) != PackageManager.PERMISSION_GRANTED) {
                    NeedsRequest = true;
                }
            }
            if (NeedsRequest) {
                ActivityCompat.requestPermissions(this, Permissions, PermissionRequestCode);
                return;
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, new String[]{ Manifest.permission.POST_NOTIFICATIONS }, PermissionRequestCode);
            return;
        }
        PopulateDeviceList();
    }

    @Override
    public void onRequestPermissionsResult(int RequestCode, String[] Permissions, int[] GrantResults) {
        super.onRequestPermissionsResult(RequestCode, Permissions, GrantResults);
        if (RequestCode == PermissionRequestCode) {
            PopulateDeviceList();
        }
    }

    private void PopulateDeviceList() {
        DeviceListLayout.removeAllViews();
        BluetoothAdapter Adapter = BluetoothAdapter.getDefaultAdapter();
        if (Adapter == null) return;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            return;
        }

        Set<BluetoothDevice> BondedDevices = Adapter.getBondedDevices();
        for (BluetoothDevice Device : BondedDevices) {
            Button DeviceButton = new Button(this);
            DeviceButton.setText(Device.getName() + "\n" + Device.getAddress());
            DeviceButton.setBackgroundColor(ColorSurface);
            DeviceButton.setTextColor(ColorTextPrimary);
            DeviceButton.setOnClickListener(View -> StartStreamService(Device.getAddress()));
            DeviceListLayout.addView(DeviceButton);
        }
    }

    private void StartStreamService(String DeviceAddress) {
        Intent ServiceIntent = new Intent(this, StreamService.class);
        ServiceIntent.putExtra("DeviceAddress", DeviceAddress);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(ServiceIntent);
        } else {
            startService(ServiceIntent);
        }
    }
}