# The JNI layer (medut_jni.c) finds these by name.
-keep class dev.medut.internal.** { *; }
-keepclassmembers class dev.medut.MedutConfig, dev.medut.AccountConfig,
                        dev.medut.OpusConfig, dev.medut.IceServer {
    <fields>;
}
-keepclassmembers enum dev.medut.** {
    int value;
}
