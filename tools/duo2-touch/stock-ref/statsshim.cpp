// libstatsshim.so: preloaded under duo2-heatref so the stock heat processor's
// telemetry finds no Android stats service at once, instead of waiting for a
// service manager that does not exist here.
//
// IStats::tryGetService(const std::string&, bool) returns android::sp<IStats>,
// a class with a destructor, so the result goes through the x8 pointer: a
// null sp is a single null pointer written there.

struct NullSp {
    void *ptr = nullptr;
    ~NullSp() {}
};

extern "C" NullSp duo2_try_get_service(const void *, bool) __asm__(
    "_ZN7android10frameworks5stats4V1_06IStats13tryGetServiceERKNSt3__112basic_"
    "stringIcNS4_11char_traitsIcEENS4_9allocatorIcEEEEb");

extern "C" __attribute__((visibility("default"))) NullSp duo2_try_get_service(const void *, bool) {
    return NullSp();
}
