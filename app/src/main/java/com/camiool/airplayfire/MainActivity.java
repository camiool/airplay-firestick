package com.camiool.airplayfire;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.TextView;

import java.net.Inet4Address;
import java.util.Locale;

public class MainActivity extends Activity implements AirPlayNative.Listener, SurfaceHolder.Callback {

    private final Handler ui = new Handler(Looper.getMainLooper());

    private Settings settings;
    private NsdPublisher nsd;
    private WifiManager.WifiLock wifiLock;
    private WifiManager.MulticastLock multicastLock;

    private AspectFrameLayout videoFrame;
    private SurfaceView surfaceView;
    private View idle;
    private TextView title, instructions, status, info, stats, toast;

    private boolean running;
    private boolean mirroring;
    private int connections;
    private String clientName;
    private long lastBackPress;
    private long lastRendered;

    private final Runnable hideToast = () -> toast.setVisibility(View.GONE);

    private final Runnable statsTick = new Runnable() {
        @Override
        public void run() {
            updateStats();
            ui.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        setContentView(R.layout.activity_main);

        settings = new Settings(this);
        nsd = new NsdPublisher(this);

        videoFrame = findViewById(R.id.videoFrame);
        surfaceView = findViewById(R.id.surface);
        idle = findViewById(R.id.idle);
        title = findViewById(R.id.title);
        instructions = findViewById(R.id.instructions);
        status = findViewById(R.id.status);
        info = findViewById(R.id.info);
        stats = findViewById(R.id.stats);
        toast = findViewById(R.id.toast);

        surfaceView.getHolder().addCallback(this);

        WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
        if (wm != null) {
            int mode = Build.VERSION.SDK_INT >= 29
                    ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                    : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            wifiLock = wm.createWifiLock(mode, "airplayfire");
            wifiLock.setReferenceCounted(false);
            multicastLock = wm.createMulticastLock("airplayfire-mdns");
            multicastLock.setReferenceCounted(false);
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (wifiLock != null) wifiLock.acquire();
        if (multicastLock != null) multicastLock.acquire();
        startReceiver();
        ui.post(statsTick);
    }

    @Override
    protected void onStop() {
        ui.removeCallbacks(statsTick);
        stopReceiver();
        if (wifiLock != null && wifiLock.isHeld()) wifiLock.release();
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        super.onStop();
    }

    // ------------------------------------------------------------------ receiver

    private void startReceiver() {
        if (running) return;
        Settings.Quality q = settings.quality();
        String keyFile = getFilesDir().getAbsolutePath() + "/airplay_key.pem";
        running = AirPlayNative.nativeStart(this, settings.name(), settings.hwAddr(),
                q.width, q.height, settings.fps(), q.h265, keyFile, settings.audio());
        if (running && surfaceView.getHolder().getSurface() != null
                && surfaceView.getHolder().getSurface().isValid()) {
            AirPlayNative.nativeSetSurface(surfaceView.getHolder().getSurface());
        }
        mirroring = false;
        connections = 0;
        clientName = null;
        refreshUi();
    }

    private void stopReceiver() {
        if (!running) return;
        AirPlayNative.nativeStop();
        running = false;
        mirroring = false;
        connections = 0;
    }

    private void restartReceiver() {
        stopReceiver();
        startReceiver();
        showToast("Ajustes aplicados. Si estabas conectado, vuelve a elegir este TV en el iPhone.");
    }

    // ------------------------------------------------------------------ UI

    private void refreshUi() {
        Settings.Quality q = settings.quality();
        String name = settings.name();
        idle.setVisibility(mirroring ? View.GONE : View.VISIBLE);
        if (!running) {
            title.setText("No se pudo iniciar el receptor AirPlay");
            instructions.setText("Revisa que el Fire TV esté conectado a Wi-Fi y vuelve a abrir la app.");
            status.setText("");
        } else {
            title.setText("Listo para AirPlay: " + name);
            instructions.setText("En tu iPhone abre el Centro de Control → Duplicar pantalla\n"
                    + "y elige «" + name + "».\n"
                    + "El iPhone y el Fire TV deben estar en la misma red Wi-Fi.");
            if (connections > 0 && clientName != null) {
                status.setText("Conectando con " + clientName + "…");
            } else if (connections > 0) {
                status.setText("Conectando…");
            } else {
                status.setText("Esperando conexión");
            }
        }
        String ip = localIp();
        info.setText("Modo: " + q.label + " · " + settings.fps() + " fps"
                + (ip != null ? "   ·   IP " + ip : "")
                + "\nBotón ☰ (menú) u OK: ajustes   ·   Atrás: salir");
        stats.setVisibility(settings.showStats() ? View.VISIBLE : View.GONE);
    }

    private void updateStats() {
        if (!settings.showStats() || !running) return;
        long[] s = AirPlayNative.nativeGetStats();
        long rendered = s[1];
        long fps = Math.max(0, rendered - lastRendered);
        lastRendered = rendered;
        String audio;
        switch ((int) s[8]) {
            case 8: audio = "AAC-ELD"; break;
            case 4: audio = "AAC"; break;
            case 2: audio = "ALAC"; break;
            default: audio = "—";
        }
        stats.setText(String.format(Locale.US,
                "%dx%d %s%s\n%d fps · decodificación %.1f ms\ndescartados %d\naudio %s · búfer %d ms · cortes %d\nconexiones %d",
                s[4], s[5], s[6] == 1 ? "H.265" : "H.264", s[7] == 1 ? " · baja latencia" : "",
                fps, s[3] / 1000.0, s[2], audio, s[9], s[10], s[11]));
    }

    private void showToast(String msg) {
        toast.setText(msg);
        toast.setVisibility(View.VISIBLE);
        ui.removeCallbacks(hideToast);
        ui.postDelayed(hideToast, 4000);
    }

    private String localIp() {
        try {
            ConnectivityManager cm = (ConnectivityManager) getSystemService(Context.CONNECTIVITY_SERVICE);
            Network n = cm.getActiveNetwork();
            if (n == null) return null;
            LinkProperties lp = cm.getLinkProperties(n);
            if (lp == null) return null;
            for (LinkAddress la : lp.getLinkAddresses()) {
                if (la.getAddress() instanceof Inet4Address) return la.getAddress().getHostAddress();
            }
        } catch (RuntimeException ignored) {
        }
        return null;
    }

    // ------------------------------------------------------------------ settings menu

    private void openSettings() {
        Settings.Quality q = settings.quality();
        String[] items = {
                "Calidad: " + q.label,
                "Cuadros por segundo: " + settings.fps(),
                "Audio en el TV: " + (settings.audio() ? "Sí" : "No"),
                "Mostrar estadísticas: " + (settings.showStats() ? "Sí" : "No"),
                "Nombre del receptor: " + settings.name(),
                "Salir de la app",
        };
        new AlertDialog.Builder(this)
                .setTitle("Ajustes")
                .setItems(items, (d, which) -> {
                    switch (which) {
                        case 0: chooseQuality(); break;
                        case 1: chooseFps(); break;
                        case 2:
                            settings.setAudio(!settings.audio());
                            restartReceiver();
                            break;
                        case 3:
                            settings.setShowStats(!settings.showStats());
                            refreshUi();
                            break;
                        case 4: editName(); break;
                        case 5: finish(); break;
                    }
                })
                .show();
    }

    private void chooseQuality() {
        Settings.Quality[] all = Settings.Quality.values();
        String[] labels = new String[all.length];
        for (int i = 0; i < all.length; i++) labels[i] = all[i].label;
        new AlertDialog.Builder(this)
                .setTitle("Calidad de imagen")
                .setSingleChoiceItems(labels, settings.quality().ordinal(), (d, which) -> {
                    d.dismiss();
                    if (all[which] != settings.quality()) {
                        settings.setQuality(all[which]);
                        restartReceiver();
                    }
                })
                .show();
    }

    private void chooseFps() {
        final int[] options = {60, 30};
        String[] labels = {"60 fps (recomendado para juegos)", "30 fps (menos exigente para el Wi-Fi)"};
        int cur = settings.fps() == 30 ? 1 : 0;
        new AlertDialog.Builder(this)
                .setTitle("Cuadros por segundo")
                .setSingleChoiceItems(labels, cur, (d, which) -> {
                    d.dismiss();
                    if (options[which] != settings.fps()) {
                        settings.setFps(options[which]);
                        restartReceiver();
                    }
                })
                .show();
    }

    private void editName() {
        EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setText(settings.name());
        input.setSelectAllOnFocus(true);
        new AlertDialog.Builder(this)
                .setTitle("Nombre que verá el iPhone")
                .setView(input)
                .setPositiveButton("Guardar", (d, w) -> {
                    String n = input.getText().toString().trim();
                    if (!n.isEmpty() && !n.equals(settings.name())) {
                        settings.setName(n.length() > 40 ? n.substring(0, 40) : n);
                        restartReceiver();
                    }
                })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    // ------------------------------------------------------------------ keys

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_MENU:
            case KeyEvent.KEYCODE_SETTINGS:
                openSettings();
                return true;
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_BUTTON_A:
                if (!mirroring) {
                    openSettings();
                    return true;
                }
                break;
            case KeyEvent.KEYCODE_BACK:
                if (mirroring) {
                    long now = System.currentTimeMillis();
                    if (now - lastBackPress > 2500) {
                        lastBackPress = now;
                        showToast("Presiona Atrás otra vez para salir");
                        return true;
                    }
                }
                finish();
                return true;
            default:
                break;
        }
        return super.onKeyDown(keyCode, event);
    }

    // ------------------------------------------------------------------ SurfaceHolder.Callback

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (running) AirPlayNative.nativeSetSurface(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        AirPlayNative.nativeSetSurface(null);
    }

    // ------------------------------------------------------------------ AirPlayNative.Listener (native threads)

    @Override
    public int registerService(String name, String type, int port, String[] keys, String[] values) {
        return nsd.register(name, type, port, keys, values);
    }

    @Override
    public void unregisterService(int handle) {
        nsd.unregister(handle);
    }

    @Override
    public void onClientRequest(String deviceName, String model) {
        ui.post(() -> {
            clientName = deviceName;
            refreshUi();
        });
    }

    @Override
    public void onConnectionsChanged(int open) {
        ui.post(() -> {
            connections = open;
            if (open == 0) {
                mirroring = false;
                clientName = null;
            }
            refreshUi();
        });
    }

    @Override
    public void onMirroringChanged(boolean active) {
        ui.post(() -> {
            mirroring = active;
            refreshUi();
        });
    }

    @Override
    public void onVideoSize(int width, int height) {
        ui.post(() -> {
            videoFrame.setAspect(width, height);
            if (!mirroring) {
                // First decoded frame: make sure the video is visible even if the
                // "mirroring started" notification has not arrived.
                mirroring = true;
                refreshUi();
            }
        });
    }
}
