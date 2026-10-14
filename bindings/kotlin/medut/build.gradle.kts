import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

// Prebuilt libmedut.so per ABI, committed under src/main/jniLibs
// (scripts/build-android.sh refreshes it); override with
// -Pmedut.jniLibsDir=... to use dist/android instead.
val jniLibsDir: String = (findProperty("medut.jniLibsDir") as String?)
    ?: projectDir.resolve("src/main/jniLibs").path
val medutInclude: String = rootDir.resolve("../../include").canonicalPath

group = "dev.medut"
version = "1.0.0"

android {
    namespace = "dev.medut"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DMEDUT_JNILIBS=$jniLibsDir",
                    "-DMEDUT_INCLUDE=$medutInclude",
                )
            }
        }
    }

    // CMake only links against the imported core; AGP does not package it,
    // so the prebuilt directory is the jniLibs source.
    sourceSets["main"].jniLibs.setSrcDirs(listOf(jniLibsDir))

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}
