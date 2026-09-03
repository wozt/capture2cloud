// AGP 9 compiles Kotlin itself; applying org.jetbrains.kotlin.android as
// well makes both try to register a "kotlin" extension and the build
// stops before it starts.
plugins {
    id("com.android.application")
}

android {
    namespace = "fr.wozt.capture2cloud"
    compileSdk = 36

    defaultConfig {
        applicationId = "fr.wozt.capture2cloud"
        minSdk = 26          // MediaCodec's Opus decoder and a sane camera-free base
        targetSdk = 36
        versionCode = 1
        versionName = "1.0.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }
    kotlin {
        jvmToolchain(21)
    }
    sourceSets["main"].kotlin.srcDir("src/main/kotlin")
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")

    /* Plain JVM tests. The parts worth testing here -- the wire format,
     * the stick shaping, the rate arithmetic -- are pure functions of
     * their inputs, so they need neither a device nor an emulator, and a
     * test that needs a phone plugged in is a test nobody runs. */
    testImplementation("junit:junit:4.13.2")
}
