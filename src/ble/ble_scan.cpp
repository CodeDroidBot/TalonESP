#include "ble_scan.h"
#include <NimBLEDevice.h>
#include <algorithm>

void bleInit() {
    NimBLEDevice::init("");
}

struct BleHit {
    String label;
    int rssi;
};

std::vector<String> bleScanDevices(uint32_t scanSeconds) {
    std::vector<String> out;

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);

    bool started = pScan->start(scanSeconds, false);
    if (!started) {
        out.push_back("(scan failed)");
        return out;
    }

    NimBLEScanResults results = pScan->getResults();
    int count = results.getCount();
    if (count <= 0) {
        out.push_back("(no BLE devices found)");
        pScan->clearResults();
        return out;
    }

    std::vector<BleHit> hits;
    for (int i = 0; i < count; i++) {
        const NimBLEAdvertisedDevice* dev = results.getDevice(i);
        if (!dev) continue;

        String name = dev->getName().length() ? String(dev->getName().c_str())
                                              : String("(unnamed)");
        String addr = String(dev->getAddress().toString().c_str());
        int rssi = dev->getRSSI();

        BleHit hit;
        hit.label = name + "  " + addr + "  " + String(rssi) + "dBm";
        hit.rssi = rssi;
        hits.push_back(hit);
    }

    std::sort(hits.begin(), hits.end(),
              [](const BleHit& a, const BleHit& b) { return a.rssi > b.rssi; });

    for (auto& h : hits) out.push_back(h.label);

    pScan->clearResults();
    return out;
}

std::vector<String> bleScanAirTags(uint32_t scanSeconds) {
    std::vector<String> out;

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(false);

    bool started = pScan->start(scanSeconds, false);
    if (!started) {
        out.push_back("(scan failed)");
        return out;
    }

    NimBLEScanResults results = pScan->getResults();
    int count = results.getCount();

    for (int i = 0; i < count; i++) {
        const NimBLEAdvertisedDevice* dev = results.getDevice(i);
        if (!dev) continue;
        if (!dev->haveManufacturerData()) continue;

        std::string mfg = dev->getManufacturerData();
        if (mfg.size() >= 3 &&
            (uint8_t)mfg[0] == 0x4C &&
            (uint8_t)mfg[1] == 0x00 &&
            (uint8_t)mfg[2] == 0x12) {
            String addr = String(dev->getAddress().toString().c_str());
            out.push_back("Possible tracker: " + addr +
                          "  " + String(dev->getRSSI()) + "dBm");
        }
    }

    pScan->clearResults();
    if (out.empty()) out.push_back("(no Find My beacons seen)");
    return out;
}

std::vector<String> bleScanSkimmers(uint32_t scanSeconds) {
    std::vector<String> out;
    static const char* suspiciousNames[] = {"HC-05", "HC-06", "HC-08", "SKIMMER"};

    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setActiveScan(true);

    bool started = pScan->start(scanSeconds, false);
    if (!started) {
        out.push_back("(scan failed)");
        return out;
    }

    NimBLEScanResults results = pScan->getResults();
    int count = results.getCount();

    for (int i = 0; i < count; i++) {
        const NimBLEAdvertisedDevice* dev = results.getDevice(i);
        if (!dev) continue;
        if (dev->getName().length() == 0) continue;

        String name = String(dev->getName().c_str());
        String upper = name;
        upper.toUpperCase();

        bool suspicious = false;
        for (auto pat : suspiciousNames) {
            if (upper.indexOf(pat) >= 0) { suspicious = true; break; }
        }
        if (suspicious) {
            String addr = String(dev->getAddress().toString().c_str());
            out.push_back("Suspicious: " + name + "  " + addr +
                          "  " + String(dev->getRSSI()) + "dBm");
        }
    }

    pScan->clearResults();
    if (out.empty()) out.push_back("(no known skimmer signatures seen)");
    return out;
}