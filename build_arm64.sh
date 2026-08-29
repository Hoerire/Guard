#!/usr/bin/env bash
set -euo pipefail

SDK=/data/user/work/android-sdk
BT=$SDK/build-tools/35.0.0
PLATFORM=$SDK/platforms/android-35/android.jar
ROOT=/data/user/work/GuardApp
BUILD=/data/user/work/build

AAPT2=$BT/aapt2
D8=$BT/d8
ZIPALIGN=$BT/zipalign
APKSIGNER=$BT/apksigner

echo "== 1/7 准备构建目录 =="
rm -rf "$BUILD"; mkdir -p "$BUILD/gen" "$BUILD/classes"

echo "== 2/7 编译资源 (aapt2 compile) =="
$AAPT2 compile --dir "$ROOT/app/src/main/res" -o "$BUILD/res.zip"

echo "== 3/7 链接资源与清单 (aapt2 link) =="
cp "$ROOT/app/src/main/AndroidManifest.xml" "$BUILD/AndroidManifest.xml"
sed -i 's#<manifest xmlns:android="http://schemas.android.com/apk/res/android">#<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.example.guard" android:versionCode="1" android:versionName="1.0">#' "$BUILD/AndroidManifest.xml"
$AAPT2 link -o "$BUILD/app.unaligned.apk" \
  -I "$PLATFORM" \
  --manifest "$BUILD/AndroidManifest.xml" \
  -A "$ROOT/app/src/main/assets" \
  --java "$BUILD/gen" \
  "$BUILD/res.zip"

echo "== 4/7 编译 Java 源码 (javac) =="
javac -source 11 -target 11 -encoding UTF-8 -classpath "$PLATFORM" -d "$BUILD/classes" \
  "$ROOT/app/src/main/java/com/example/guard/MainActivity.java" \
  "$BUILD/gen/com/example/guard/R.java"

echo "== 5/7 生成 dex (d8) =="
$D8 --release --lib "$PLATFORM" --min-api 26 --output "$BUILD" \
  "$BUILD"/classes/com/example/guard/*.class

echo "== 6/7 打包 classes.dex 进 APK =="
cd "$BUILD"
zip -q app.unaligned.apk classes.dex
cd /

echo "== 7/7 对齐并签名 =="
$ZIPALIGN -f 4 "$BUILD/app.unaligned.apk" "$BUILD/app.aligned.apk"
KEYSTORE="/workspace/guard.keystore"
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -noprompt -keystore "$KEYSTORE" -alias guard \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass android -keypass android \
    -dname "CN=Guard, OU=Guard, O=Guard, L=N, S=N, C=CN"
fi
$APKSIGNER sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
  --out "$BUILD/GuardApp-arm64-v8a.apk" "$BUILD/app.aligned.apk"

echo "== 校验 =="
$APKSIGNER verify --print-certs "$BUILD/GuardApp-arm64-v8a.apk" | head -5
echo "== DONE =="
ls -l "$BUILD/GuardApp-arm64-v8a.apk"