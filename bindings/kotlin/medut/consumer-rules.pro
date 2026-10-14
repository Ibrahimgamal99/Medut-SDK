# The JNI layer (medut_jni.c) finds these by name.
-keep class dev.medut.internal.** { *; }

# It reads the config objects' fields with GetFieldID, and a field's
# signature names its type ("Ldev/medut/Transport;"): the config classes and
# every enum they hold must keep their names, their fields, and their being
# enums (R8 would otherwise unbox an enum field into an int).  Keeping only
# the fields let R8 rename dev.medut.Transport, and MedutAndroid.start failed
# in every minified build with NoSuchFieldError.
-keep class dev.medut.MedutConfig, dev.medut.AccountConfig,
            dev.medut.OpusConfig, dev.medut.IceServer {
    <fields>;
}
-keep enum dev.medut.** { *; }
