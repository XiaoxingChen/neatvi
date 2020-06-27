WORK_DIR=`pwd`
BUILD_DIR=build
ANDROID_DIR=/data/local/tmp

mkdir -p build
cd $BUILD_DIR
cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake
make -j4

adb push vi $ANDROID_DIR