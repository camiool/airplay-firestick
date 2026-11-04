package com.camiool.airplayfire;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.util.Log;

import java.util.HashMap;
import java.util.Map;

/** Publishes the AirPlay (_airplay._tcp) and RAOP (_raop._tcp) services via Android's mDNS. */
final class NsdPublisher {
    private static final String TAG = "AirPlayFire";

    private final NsdManager nsd;
    private final Map<Integer, NsdManager.RegistrationListener> listeners = new HashMap<>();
    private int nextHandle = 1;

    NsdPublisher(Context ctx) {
        nsd = (NsdManager) ctx.getSystemService(Context.NSD_SERVICE);
    }

    synchronized int register(String name, String type, int port, String[] keys, String[] values) {
        NsdServiceInfo info = new NsdServiceInfo();
        info.setServiceName(name);
        info.setServiceType(type);
        info.setPort(port);
        for (int i = 0; i < keys.length; i++) {
            try {
                info.setAttribute(keys[i], values[i]);
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "TXT attribute rejected: " + keys[i], e);
            }
        }
        NsdManager.RegistrationListener l = new NsdManager.RegistrationListener() {
            @Override
            public void onRegistrationFailed(NsdServiceInfo s, int errorCode) {
                Log.e(TAG, "mDNS registration failed for " + type + ": " + errorCode);
            }

            @Override
            public void onUnregistrationFailed(NsdServiceInfo s, int errorCode) {
                Log.w(TAG, "mDNS unregistration failed for " + type + ": " + errorCode);
            }

            @Override
            public void onServiceRegistered(NsdServiceInfo s) {
                Log.i(TAG, "mDNS registered: " + s.getServiceName() + " " + type);
            }

            @Override
            public void onServiceUnregistered(NsdServiceInfo s) {
                Log.i(TAG, "mDNS unregistered: " + type);
            }
        };
        try {
            nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, l);
        } catch (RuntimeException e) {
            Log.e(TAG, "registerService threw", e);
            return 0;
        }
        int h = nextHandle++;
        listeners.put(h, l);
        return h;
    }

    synchronized void unregister(int handle) {
        NsdManager.RegistrationListener l = listeners.remove(handle);
        if (l == null) return;
        try {
            nsd.unregisterService(l);
        } catch (RuntimeException e) {
            Log.w(TAG, "unregisterService threw", e);
        }
    }
}
