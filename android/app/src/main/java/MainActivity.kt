package org.sdrpp.sdrpp;

import android.app.NativeActivity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.hardware.usb.*;
import android.Manifest;
import android.os.Bundle;
import android.view.View;
import android.view.KeyEvent;
import android.view.inputmethod.InputMethodManager;
import android.util.Log;
import android.content.res.AssetManager;

import androidx.core.app.ActivityCompat;

import androidx.core.content.PermissionChecker;

import java.util.concurrent.LinkedBlockingQueue;
import java.io.*;

private const val ACTION_USB_PERMISSION = "org.sdrpp.sdrpp.USB_PERMISSION";

// USB permission results. Stays registered: the old one unregistered itself after the
// first answer, so a second device (or a re-plugged one) could never be opened.
private val usbReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (ACTION_USB_PERMISSION == intent.action) {
            synchronized(this) {
                var _this = context as MainActivity;
                val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (dev != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    _this.openSdr(dev);
                }

                // Hide again the system bars
                _this.hideSystemBars();
            }
        }
    }
}

// SDR plugged in while the app is running: ask for permission right away (it then
// auto-starts); unplugged: forget it.
private val usbAttachReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            var _this = context as MainActivity;
            val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            if (dev != null) { _this.checkUartBridge(dev, true); _this.requestSdrPermission(dev); }
        }
        else if (UsbManager.ACTION_USB_DEVICE_DETACHED == intent.action) {
            var _this = context as MainActivity;
            val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            if (dev != null) { _this.checkUartBridge(dev, false); _this.sdrDetached(dev); }
        }
    }
}

class MainActivity : NativeActivity() {
    private val TAG : String = "SDR++";
    public var usbManager : UsbManager? = null;
    public var SDR_device : UsbDevice? = null;
    public var SDR_conn : UsbDeviceConnection? = null;
    public var SDR_VID : Int = -1;
    public var SDR_PID : Int = -1;
    public var SDR_FD : Int = -1;
    // USB-UART bridge seen ((vid shl 16) or pid), 0 if none: ESP32-S3 DevKit plugged into its UART port
    public var UART_HINT : Int = 0;
    private var uartHintDev : String? = null;

    private val UART_BRIDGES = setOf(
        (0x1a86 shl 16) or 0x55d3, (0x1a86 shl 16) or 0x55d4, (0x1a86 shl 16) or 0x7523,   // WCH CH343 / CH9102 / CH340
        (0x10c4 shl 16) or 0xea60,                                                          // Silicon Labs CP210x
        (0x0403 shl 16) or 0x6001, (0x0403 shl 16) or 0x6010, (0x0403 shl 16) or 0x6014, (0x0403 shl 16) or 0x6015)  // FTDI

    public fun checkUartBridge(dev: UsbDevice, attached: Boolean) {
        val id = (dev.getVendorId() shl 16) or dev.getProductId();
        if (!UART_BRIDGES.contains(id)) { return; }
        if (attached) {
            uartHintDev = dev.getDeviceName();
            UART_HINT = id;
            flog("UART bridge plugged in: " + dev.getDeviceName() + String.format(" %04x:%04x", dev.getVendorId(), dev.getProductId()));
        }
        else if (uartHintDev == dev.getDeviceName()) {
            uartHintDev = null;
            UART_HINT = 0;
        }
    }

    // Field log: appended to the native log file (Download/sdrpp-log.txt), readable over MTP
    fun flog(msg: String) {
        Log.i(TAG, msg);
        try {
            val ts = java.text.SimpleDateFormat("HH:mm:ss.SSS", java.util.Locale.US).format(java.util.Date());
            File("/storage/emulated/0/Download/sdrpp-log.txt").appendText("[" + ts + "] [JAVA] " + msg + "\n");
        } catch (e: Exception) {}
    }

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1);
        }
    }

    public var permissionIntent : PendingIntent? = null;

    // Supported SDR VID/PIDs (res/xml/device_filter.xml): only these are opened, so a
    // hub, keyboard or charger on the same OTG port can never replace the SDR's fd.
    private var sdrIds : HashSet<Int>? = null;

    fun isSdr(dev: UsbDevice): Boolean {
        if (sdrIds == null) {
            val ids = HashSet<Int>();
            try {
                val xml = getResources().getXml(R.xml.device_filter);
                var ev = xml.getEventType();
                while (ev != org.xmlpull.v1.XmlPullParser.END_DOCUMENT) {
                    if (ev == org.xmlpull.v1.XmlPullParser.START_TAG && xml.getName() == "usb-device") {
                        val v = xml.getAttributeIntValue(null, "vendor-id", -1);
                        val p = xml.getAttributeIntValue(null, "product-id", -1);
                        ids.add((v shl 16) or p);
                    }
                    ev = xml.next();
                }
            } catch (e: Exception) { Log.e(TAG, "device_filter: " + e); }
            sdrIds = ids;
        }
        return sdrIds!!.contains((dev.getVendorId() shl 16) or dev.getProductId());
    }

    // Open the SDR once. The attach broadcast, onNewIntent, the permission answer and the
    // start-up scan can all report the same device: opening it again would hand the native
    // side a second fd for a device that is already streaming, and the restart that follows
    // (old fd still claimed) froze the app right after the first audio.
    @Synchronized
    public fun openSdr(dev: UsbDevice) {
        if (!isSdr(dev)) { return; }
        if (SDR_conn != null && SDR_device?.getDeviceName() == dev.getDeviceName()) {
            flog("openSdr: " + dev.getDeviceName() + " already open (fd " + SDR_FD + ")");
            return;
        }
        val conn = usbManager!!.openDevice(dev);
        if (conn == null) { flog("openSdr: openDevice failed for " + dev.getDeviceName()); return; }
        flog("openSdr: " + dev.getDeviceName() + String.format(" %04x:%04x", dev.getVendorId(), dev.getProductId()) + " fd " + conn.getFileDescriptor());
        SDR_device = dev;
        SDR_conn = conn;
        SDR_VID = dev.getVendorId();
        SDR_PID = dev.getProductId();
        SDR_FD = conn.getFileDescriptor();
    }

    // The SDR was unplugged: forget it, so the next plug-in opens (and auto-starts) again.
    // The connection itself is left to the native side, which may still be closing it.
    @Synchronized
    public fun sdrDetached(dev: UsbDevice) {
        if (SDR_device?.getDeviceName() != dev.getDeviceName()) { return; }
        flog("SDR detached: " + dev.getDeviceName());
        SDR_device = null;
        SDR_conn = null;
        SDR_FD = -1;
        SDR_VID = -1;
        SDR_PID = -1;
    }

    public fun requestSdrPermission(dev: UsbDevice) {
        if (!isSdr(dev)) { return; }
        flog("requestSdrPermission: " + dev.getDeviceName() + " hasPermission " + usbManager!!.hasPermission(dev));
        if (usbManager!!.hasPermission(dev)) {
            openSdr(dev);
        }
        else {
            usbManager!!.requestPermission(dev, permissionIntent);
        }
    }

    // Launched (or brought back, launchMode=singleTask) by plugging in a supported SDR:
    // the system has already granted permission for that device.
    public override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent);
        flog("onNewIntent: " + intent.action);
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            val dev: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            if (dev != null) { requestSdrPermission(dev); }
        }
        hideSystemBars();
    }

    public fun hideSystemBars() {
        val decorView = getWindow().getDecorView();
        val uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decorView.setSystemUiVisibility(uiOptions);
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        flog("onCreate (saved state " + (savedInstanceState != null) + ", intent " + getIntent()?.action + ")");
        // Hide bars
        hideSystemBars();

        // Ask for required permissions, without these the app cannot run.
        checkAndAsk(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        checkAndAsk(Manifest.permission.READ_EXTERNAL_STORAGE);

        // TODO: Have the main code wait until these two permissions are available

        // Register events
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager;
        permissionIntent = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION), 0)
        registerReceiver(usbReceiver, IntentFilter(ACTION_USB_PERMISSION))
        val attachFilter = IntentFilter(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        attachFilter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        registerReceiver(usbAttachReceiver, attachFilter)

        // Get permission for all USB devices already plugged in
        val devList = usbManager!!.getDeviceList();
        for ((name, dev) in devList) {
            checkUartBridge(dev, true);
            requestSdrPermission(dev);
        }

        // Ask for internet permission
        checkAndAsk(Manifest.permission.INTERNET);

        super.onCreate(savedInstanceState)
    }

    public override fun onPause() {
        flog("onPause");
        super.onPause();
    }

    public override fun onDestroy() {
        flog("onDestroy (finishing " + isFinishing() + ")");
        try { unregisterReceiver(usbReceiver); } catch (e: Exception) {}
        try { unregisterReceiver(usbAttachReceiver); } catch (e: Exception) {}
        super.onDestroy();
    }

    public override fun onResume() {
        flog("onResume");
        // Hide bars again
        hideSystemBars();
        super.onResume();
    }

    fun showSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.showSoftInput(window.decorView, 0);
    }

    fun hideSoftInput() {
        val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager;
        inputMethodManager.hideSoftInputFromWindow(window.decorView.windowToken, 0);
        hideSystemBars();
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // We assume dispatchKeyEvent() of the NativeActivity is actually called for every
    // KeyEvent and not consumed by any View before it reaches here
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            unicodeCharacterQueue.offer(event.getUnicodeChar(event.metaState))
        }
        return super.dispatchKeyEvent(event)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc);
        var ecount = 0;
        for (fp in flist) {
            val lpath = local + "/" + fp;
            val rpath = rsrc + "/" + fp;

            Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

            // Create local path if non-existent
            createIfDoesntExist(local);
            
            // Create if directory
            val ext = extractDir(aman, lpath, rpath);

            // Extract if file
            if (ext == 0) {
                // This is a file, extract it
                val _os = FileOutputStream(lpath);
                val _is = aman.open(rpath);
                val ilen = _is.available();
                var fbuf = ByteArray(ilen);
                _is.read(fbuf, 0, ilen);
                _os.write(fbuf);
                _os.close();
                _is.close();
            }

            ecount++;
        }
        return ecount;
    }

    // Debug report for bug reports (Settings > Export debug log): device, USB and app details,
    // the last two log files and the configuration, shared through the Android share sheet
    // (mail, messengers, Drive, ...). header: SDR++ version and build line from the native side.
    public fun shareDebugReport(header: String) {
        try {
            val dir = File(getCacheDir(), "reports");
            dir.mkdirs();
            val stamp = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US).format(java.util.Date());
            val out = File(dir, "z2sdr-report-" + stamp + ".txt");
            dir.listFiles()?.forEach { if (it != out) it.delete(); }
            val sb = StringBuilder();
            sb.append("Z2 SDR debug report " + stamp + "\n");
            sb.append(header + "\n");
            try {
                val pi = getPackageManager().getPackageInfo(getPackageName(), 0);
                sb.append("APK: " + getPackageName() + " " + pi.versionName + "\n");
            } catch (e: Exception) {}
            sb.append("Device: " + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL +
                      " (" + android.os.Build.DEVICE + "), Android " + android.os.Build.VERSION.RELEASE +
                      " (SDK " + android.os.Build.VERSION.SDK_INT + ")\n");
            sb.append("USB devices:\n");
            try {
                val um = getSystemService(Context.USB_SERVICE) as UsbManager;
                for (d in um.getDeviceList().values) {
                    sb.append(String.format("  %04x:%04x %s %s/%s permission=%b\n", d.getVendorId(), d.getProductId(),
                              d.getDeviceName(), d.getManufacturerName() ?: "?", d.getProductName() ?: "?", um.hasPermission(d)));
                }
            } catch (e: Exception) { sb.append("  (" + e + ")\n"); }
            val logDir = "/storage/emulated/0/Download/";
            for (name in arrayOf("sdrpp-log.2.txt", "sdrpp-log.1.txt", "sdrpp-log.txt")) {
                val f = File(logDir + name);
                if (!f.exists()) continue;
                sb.append("\n===== " + name + " (" + f.length() + " bytes) =====\n");
                sb.append(tail(f, 3 * 1024 * 1024));
            }
            val conf = File(getFilesDir(), "config.json");
            if (conf.exists()) {
                sb.append("\n===== config.json =====\n");
                sb.append(tail(conf, 512 * 1024));
            }
            out.writeText(sb.toString());
            val uri = androidx.core.content.FileProvider.getUriForFile(this, getPackageName() + ".reports", out);
            val send = Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.putExtra(Intent.EXTRA_SUBJECT, "Z2 SDR debug report " + stamp);
            send.putExtra(Intent.EXTRA_TEXT, header + "\n" + android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL +
                          ", Android " + android.os.Build.VERSION.RELEASE + "\n\nWhat happened:\n");
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            flog("debug report: " + out.getName() + " (" + out.length() + " bytes)");
            runOnUiThread { startActivity(Intent.createChooser(send, "Send the Z2 SDR debug report")); };
        } catch (e: Exception) {
            flog("debug report failed: " + e);
        }
    }

    // Last maxBytes of a file as text (the start of a long log matters less than the end)
    private fun tail(f: File, maxBytes: Int): String {
        val len = f.length();
        return RandomAccessFile(f, "r").use { r ->
            val start = if (len > maxBytes) len - maxBytes else 0L;
            r.seek(start);
            val b = ByteArray((len - start).toInt());
            r.readFully(b);
            val text = String(b, Charsets.UTF_8);
            if (start > 0) "[... " + start + " bytes left out ...]\n" + text else text
        }
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        // Extract all resources to the app directory
        val aman = getAssets();
        extractDir(aman, fdir + "/res", "res");
        createIfDoesntExist(fdir + "/modules");

        return fdir;
    }
}
