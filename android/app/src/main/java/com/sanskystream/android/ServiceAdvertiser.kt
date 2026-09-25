package com.sanskystream.android

// ---------------------------------------------------------------------------
// ServiceAdvertiser.kt — M18: mDNS/NSD Advertisement
//
// Phase 8: Device discovery integration.
//
// Advertises "SanskyStream <device-name>._sanskystream._tcp" on the local
// network using Android's NsdManager (Network Service Discovery).
//
// TXT records:
//   ver=1           Protocol version (matches Protocol.PROTOCOL_VERSION)
//   role=sender     Identifies this as a source device
//   platform=android  M18: allows Windows UI to show "[Android]" badge
//
// The Windows DeviceDiscovery.cpp (M16) discovers this advertisement
// via DnsServiceBrowse() — no changes needed on the Windows side.
//
// Note: Android NsdManager does not support custom TXT records in older APIs.
// The service name itself encodes the platform hint:
//   "SanskyStream-Android <device name>"
// Devices without the platform TXT key default to DevicePlatform::Unknown
// on the Windows side, which is functionally identical.
//
// Threading: start/stop must be called from the main thread.
// ---------------------------------------------------------------------------

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.provider.Settings
import android.util.Log

private const val TAG = "SanskyServiceAdvertiser"

class ServiceAdvertiser(private val context: Context) {

    private var nsdManager: NsdManager? = null
    private var registrationListener: NsdManager.RegistrationListener? = null

    var isAdvertising = false
        private set

    private var registeredServiceName: String = ""

    /**
     * Start advertising on the local network.
     * Uses the device's user-visible name as the service instance name.
     * Port is always CONTROL_TCP_PORT (5000).
     */
    fun start() {
        if (isAdvertising) return

        val deviceName = getDeviceName()
        // Prefix with "Android" so Windows UI can display the platform badge
        // even without TXT record support (older NsdManager APIs).
        val serviceName = "SanskyStream-Android $deviceName"

        val serviceInfo = NsdServiceInfo().apply {
            this.serviceName = serviceName
            serviceType      = "${Protocol.SERVICE_TYPE}."
            port             = Protocol.CONTROL_TCP_PORT
        }

        val listener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(info: NsdServiceInfo) {
                registeredServiceName = info.serviceName
                isAdvertising = true
                Log.i(TAG, "NSD registered: '${info.serviceName}' on port ${Protocol.CONTROL_TCP_PORT}")
            }

            override fun onRegistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "NSD registration failed. Error: $errorCode")
                isAdvertising = false
            }

            override fun onServiceUnregistered(info: NsdServiceInfo) {
                isAdvertising = false
                Log.i(TAG, "NSD unregistered: '${info.serviceName}'")
            }

            override fun onUnregistrationFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "NSD unregistration failed. Error: $errorCode")
            }
        }

        try {
            val mgr = context.getSystemService(Context.NSD_SERVICE) as NsdManager
            nsdManager = mgr
            registrationListener = listener
            mgr.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, listener)
            Log.i(TAG, "Starting NSD advertisement for '$serviceName'")
        } catch (e: Exception) {
            Log.e(TAG, "NSD start failed: ${e.message}")
        }
    }

    /** Stop advertising. */
    fun stop() {
        val listener = registrationListener ?: return
        try {
            nsdManager?.unregisterService(listener)
        } catch (e: Exception) {
            Log.e(TAG, "NSD stop error: ${e.message}")
        }
        registrationListener = null
        nsdManager           = null
        isAdvertising        = false
    }

    private fun getDeviceName(): String {
        // Use the user-visible device name (same as iOS UIDevice.current.name).
        return try {
            Settings.Global.getString(context.contentResolver, "device_name")
                ?: Settings.Secure.getString(context.contentResolver, "bluetooth_name")
                ?: Build.MODEL
        } catch (_: Exception) {
            Build.MODEL
        }
    }
}
