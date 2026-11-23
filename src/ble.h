#pragma once
#include <Arduino.h>

// BLE peripheral "OTTO". Otto has no car radio: the only central is the browser tuning page
// (tools/tuning.html) for settings, live data, the drive log and wireless firmware updates.
// Commands arrive as text on the CMD characteristic (see commands.cpp).

void bleInit();
void bleUpdate(uint32_t now);
bool bleAnyConnected();
bool bleTunerConnected();        // the tuning page (subscribed); a paired phone alone doesn't count
const char* blePhoneStatus();    // last phone clock result (`phone`)
void bleNotifyTelemetry(const char* json);
void bleStartLogStream();
void bleReply(uint16_t conn, const char* msg);   // sent as {"msg":...} on telemetry

// Advertising: fast for 30 s after bleInit (each wake), a disconnect or bleOpenWindow() (menu), then slow.
bool bleReady();                 // stack running (off when wireless and OBD are both off)
void bleOpenWindow();            // advertise fast for the next 30 s
bool bleAdvertising();
void bleTestAdvertising(int mode);   // pwrtest: -1 normal, 0 off, 1 slow, 2 fast
void bleShutdown();              // pwrtest: stack off until the next restart
