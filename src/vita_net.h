#pragma once

class VitaNet {
public:
    VitaNet();
    ~VitaNet();
    bool init();
    void shutdown();
    const char* error() const { return error_; }
    void poll();
    // Re-render the last device snapshot after a catalog change, without polling hardware.
    void refreshLanguage();
    const char* status() const { return status_; }
private:
    void* memory_;
    bool net_ready_;
    bool ctl_ready_;
    int flight_ = -1, wifi_ = -1, state_ = -1;
    char error_[128];
    char status_[160] = "Network status unknown";
};
