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
        versionCode = 2
        versionName = "1.1.0"
    }

    /* The release key lives outside the repository and is never
     * committed: an app signed with a key in a public tree is an app
     * anybody can publish an update for. Its absence is not an error --
     * a debug build needs none -- so a checkout without it still builds
     * everything but the signed release APK. */
    val keystore = File(System.getProperty("user.home"), ".android/capture2cloud-signing/release.jks")
    val keystorePassword = File(System.getProperty("user.home"), ".android/capture2cloud-signing/password")

    signingConfigs {
        if (keystore.exists() && keystorePassword.exists()) {
            create("release") {
                val pw = keystorePassword.readText().trim()
                storeFile = keystore
                storePassword = pw
                keyAlias = "capture2cloud"
                keyPassword = pw
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (keystore.exists() && keystorePassword.exists()) {
                signingConfig = signingConfigs.getByName("release")
            }
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
