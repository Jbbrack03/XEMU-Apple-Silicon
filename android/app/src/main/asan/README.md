# ASan diagnostic build support (s42)

`./gradlew assembleRelease -PxemuAsan` builds libxemu with ASan
(CMake -DXEMU_ASAN=ON) and packages res/lib/arm64-v8a/wrap.sh so the
runtime is LD_PRELOADed (app is debuggable).

Before building, copy the NDK ASan runtime here (not committed):

    cp $NDK/toolchains/llvm/prebuilt/*/lib/clang/*/lib/linux/libclang_rt.asan-aarch64-android.so \
       android/app/src/main/asan/jniLibs/arm64-v8a/

Without -PxemuAsan this directory is inert (property-gated sourceSets).
