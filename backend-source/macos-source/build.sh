#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build/AudioExclusive.app/Contents/MacOS
xcrun clang++ -std=c++20 -O2 -fobjc-arc -Wall -Wextra -Werror -mmacosx-version-min=14.2 -arch arm64 -arch x86_64 AudioExclusive.mm -framework Cocoa -framework CoreAudio -o build/AudioExclusive.app/Contents/MacOS/AudioExclusive
cp Info.plist build/AudioExclusive.app/Contents/Info.plist
codesign --force --sign - build/AudioExclusive.app
codesign --verify --strict build/AudioExclusive.app
build/AudioExclusive.app/Contents/MacOS/AudioExclusive --self-test
lipo -info build/AudioExclusive.app/Contents/MacOS/AudioExclusive
ditto -c -k --keepParent build/AudioExclusive.app build/AudioExclusive-macOS-universal.zip
