WORK_DIR=`pwd`
BUILD_DIR=build
ADB_ROOT=/data/local/tmp
ANDROID_DIR=$ADB_ROOT/usr/bin/

mkdir -p build
cd $BUILD_DIR
cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake
make -j4
adb shell "mkdir -p $ANDROID_DIR"
adb push vi $ANDROID_DIR