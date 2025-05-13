// SRT specific options
#ifdef HAS_SRT
        op.add<Value<int>>("", "srt-latency", "SRT latency in milliseconds", 120, &settings.server.srt.latency);
        op.add<Switch>("", "srt-encryption", "Enable SRT encryption", &settings.server.srt.encryption);
        op.add<Value<string>>("", "srt-passphrase", "SRT encryption passphrase", "", &settings.server.srt.passphrase);
#endif
