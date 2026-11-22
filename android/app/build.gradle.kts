plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "dev.pdisp"
    compileSdk = 36
    buildToolsVersion = "36.0.0"
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "dev.pdisp"
        minSdk = 29
        targetSdk = 36
        versionCode = (System.getenv("PDISP_VERSION_CODE") ?: "1").toInt() // CI passes the run number
        versionName = "0.2"
        ndk { abiFilters += listOf("arm64-v8a") }
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=none" } }
    }
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
    kotlin { compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) } }
    // Release signing comes from the environment (CI secrets, or your shell): PDISP_KEYSTORE (a path), PDISP_KEYSTORE_PASSWORD, PDISP_KEY_ALIAS,
    // PDISP_KEY_PASSWORD. Without them the release build is signed with the local debug key, which is fine for trying it but changes per machine,
    // so such an APK cannot be updated in place by one from another machine (see docs/app.md, "Signing").
    val releaseKeystore = System.getenv("PDISP_KEYSTORE")
    signingConfigs {
        if (releaseKeystore != null) create("release") {
            storeFile = file(releaseKeystore)
            storePassword = System.getenv("PDISP_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("PDISP_KEY_ALIAS")
            keyPassword = System.getenv("PDISP_KEY_PASSWORD")
        }
    }
    buildTypes {
        release {
            signingConfig = if (releaseKeystore != null) signingConfigs.getByName("release") else signingConfigs.getByName("debug")
        }
    }
    buildFeatures { compose = true }
    packaging { resources { excludes += "/META-INF/{AL2.0,LGPL2.1}" } }
}

dependencies {
    implementation(platform("androidx.compose:compose-bom:2025.06.01"))
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.core:core-ktx:1.16.0")
}
