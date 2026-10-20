plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// 仓库根（tools/android 往上两级）—— native 构建靠它定位 moqlite/ quic/ tls/ 等。
val moqRepoRoot: String = rootProject.projectDir.parentFile.parentFile.absolutePath

android {
    namespace = "com.example.moqlitepush"

    // 显式钉住 NDK 版本。不写的话 AGP 会用它自带的默认版本，本机没装那个版本时
    // 会去下载一个 —— 而本机装的是 30.0.16248370。
    ndkVersion = "30.0.16248370"

    compileSdk {
        version = release(37)
    }

    defaultConfig {
        applicationId = "com.example.moqlitepush"
        minSdk = 24
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // 只出 arm64-v8a。加别的 ABI 之前要先给那个 ABI 编一份 OpenSSL
        // （bash tools/android/build_openssl_android.sh <abi>），否则 CMake 会
        // 明确报「OpenSSL for <abi> not found」。
        ndk {
            abiFilters += "arm64-v8a"
        }

        // MOQ_ROOT 交给 app/src/main/cpp/CMakeLists.txt 定位整个仓库。
        externalNativeBuild {
            cmake {
                arguments += "-DMOQ_ROOT=$moqRepoRoot"
            }
        }
    }

    buildTypes {
        release {
            optimization {
                enable = true
                packageScope = setOf("androidx.**", "kotlin.**", "kotlinx.**")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        compose = true
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")

            // 用 SDK 里**已经装好的**那个 CMake，别让 AGP 去下它默认的 3.22.1。
            // 本机 SDK 的 3.22.1 只有一个 `.installer/` 残目录（下载反复中断留下的），
            // 于是每次 sync 都重下一遍、每次都在同一处断 —— 报的就是
            // "Failed to install the following SDK components: cmake;3.22.1"。
            // 4.1.2 是完整的（bin/cmake --version = 4.1.2，自带 ninja）。
            // 兼容性已实测：libuv 声明的 cmake_minimum_required(VERSION 3.5)
            // 在 4.1.2 下能正常 configure（CMake 4 只对 **< 3.5** 报错）。
            version = "4.1.2"
        }
    }
}

dependencies {
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    testImplementation(libs.junit)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)
}