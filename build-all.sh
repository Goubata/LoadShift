#!/bin/sh
# Build native lib + lab, the Zygisk module ZIP and the manager APK.
#
# Needs: ANDROID_HOME (SDK with platform 37 + NDK), JAVA_HOME (JDK 17), clang/lld, python3.
# Optional: GRADLE (default ./gradlew), GRADLE_ARGS (e.g. --offline).
# Machine-specific values can go in ./build.env (git-ignored), which is sourced if present.
set -e
cd "$(dirname "$0")"
[ -f build.env ] && . ./build.env
: "${ANDROID_HOME:?set ANDROID_HOME}"
: "${JAVA_HOME:?set JAVA_HOME}"
export ANDROID_HOME JAVA_HOME
python3 -I build-native.py
rm -rf build/module && mkdir -p build/module/zygisk
cp module/* build/module/
cp build/native/libloadshift.so build/module/zygisk/arm64-v8a.so
python3 -I -c "import shutil; shutil.make_archive('build/LoadShift-module', 'zip', 'build/module')"
[ -f local.properties ] || echo "sdk.dir=$ANDROID_HOME" > local.properties
${GRADLE:-./gradlew} -q ${GRADLE_ARGS:-} assembleDebug
cp app/build/outputs/apk/debug/app-debug.apk build/LoadShift.apk
ls -la build/*.zip build/*.apk
