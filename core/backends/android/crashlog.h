#pragma once

// Field diagnostics for the Android build, readable without adb:
//  - every flog line is also appended to Download/sdrpp-log.txt (previous runs are kept as
//    sdrpp-log.1.txt and sdrpp-log.2.txt, so a crash survives the relaunch);
//  - fatal signals write the signal, fault address and a symbolised backtrace there;
//  - a watchdog notices a stalled UI thread (> 2 s without a frame) and writes its stack.
namespace crashlog {
    // Opens the log file and installs the signal handlers and the watchdog. Call once, early.
    void init();

    // Called by the UI thread once per frame (watchdog heartbeat).
    void frame();

    // Short description of what the UI thread is doing right now (string literal / static
    // storage only). Shown when the watchdog fires.
    void step(const char* what);
}
