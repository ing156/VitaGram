#pragma once

class VitaNet {
public:
    VitaNet();
    ~VitaNet();
    bool init();
    void shutdown();
    const char* error() const { return error_; }
    void poll();
    const char* status() const { return status_; }
private:
    void* memory_;
    bool net_ready_;
    bool ctl_ready_;
    char error_[128];
    char status_[160] = "Network status unknown";
};
