bool MkwNativeModelSubmit(CpuContext* ctx) {
    if (!RegionAvailable()) { ++declined; return false; }
    // All original checked access/dispatch paths remain inside the region.
    // No speculative memory resolution or pointer crosses a guest call.
    ++accepted;
    Model_80064FD0(ctx);
    return true;
}
void MkwReportNativeModel() {
    std::fprintf(stderr, "HOS native model region: submitted=%llu fallback=%llu\n",
                 (unsigned long long)accepted, (unsigned long long)declined);
    const auto cache = NativeModelCache::counts;
    std::fprintf(stderr, "HOS native model cache: textures=%llu/%llu tluts=%llu/%llu copies=%llu copy-fallback=%llu\n",
                 (unsigned long long)cache.textureHits, (unsigned long long)cache.textures,
                 (unsigned long long)cache.tlutHits, (unsigned long long)cache.tluts,
                 (unsigned long long)cache.copies, (unsigned long long)cache.copyFallback);
    accepted = declined = 0;
    NativeModelCache::counts = {};
}
