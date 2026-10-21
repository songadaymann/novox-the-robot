#!/usr/bin/env bash
# Builds the macOS release of Novox The Robot: one installer (.pkg) with the standalone app, the
# Audio Unit and the VST3, for Apple Silicon Macs on macOS 13.3 or later (whisper.cpp uses Accelerate calls new in 13.3).
#
#   scripts/package-mac.sh
#
# Output in out/release/: Novox-The-Robot-macOS.pkg (the name the download page links to) and
# SHA256SUMS. The build goes to build-release/ and never touches the plugins installed in ~/Library.
#
# Signing and notarization run when these are set (see packaging/macos/README.md); without them the
# bundles are signed ad hoc and the installer is unsigned, which macOS opens only through
# System Settings > Privacy & Security > Open Anyway:
#   NOVOX_SIGN_APP        "Developer ID Application: Jonathan Mann (TEAMID)"
#   NOVOX_SIGN_INSTALLER  "Developer ID Installer: Jonathan Mann (TEAMID)"
#   NOVOX_NOTARY_PROFILE  a notarytool keychain profile (xcrun notarytool store-credentials)
# NOVOX_JUCE_SOURCE / NOVOX_WHISPER_SOURCE: existing JUCE and (patched) whisper.cpp checkouts to
# build from instead of fetching them again (e.g. build-plugin/_deps/juce-src).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-release"
OUT="$ROOT/out/release"
PACKAGING="$ROOT/packaging/macos"
PRODUCT="Novox The Robot"
IDENTIFIER="com.songaday.novox"
MIN_MACOS="13.3"

VERSION="$(sed -n 's/^project(NovoxTheRobot VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
LABEL="$(sed -n 's/^set(NOVOX_VERSION_LABEL "\([^"]*\)".*/\1/p' "$ROOT/CMakeLists.txt")"
[[ -n "$VERSION" && -n "$LABEL" ]] || { echo "Could not read the version from CMakeLists.txt" >&2; exit 1; }
echo "== Novox The Robot $LABEL ($VERSION)"

# 1. Build. GGML_NATIVE=OFF: whisper.cpp otherwise compiles for this Mac's own chip, which an older
# Apple Silicon Mac may not run; armv8.2-a with dot product and fp16 is every M-series chip.
EXTRA=()
[[ -n "${NOVOX_JUCE_SOURCE:-}" ]] && EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_JUCE=$NOVOX_JUCE_SOURCE")
[[ -n "${NOVOX_WHISPER_SOURCE:-}" ]] && EXTRA+=("-DFETCHCONTENT_SOURCE_DIR_WHISPER=$NOVOX_WHISPER_SOURCE")
# The SDK is named every time: a cache keeps the one it was first configured with, which a
# Command Line Tools update removes (JUCE's juceaide then can't find TargetConditionals.h).
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DNOVOX_BUILD_PLUGIN=ON \
  -DNOVOX_INSTALL_AFTER_BUILD=OFF -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
  -DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16 ${EXTRA[@]+"${EXTRA[@]}"} >/dev/null
cmake --build "$BUILD" --target NovoxTheRobot_AU NovoxTheRobot_VST3 NovoxTheRobot_Standalone -j 8

ARTEFACTS="$BUILD/plugin/NovoxTheRobot_artefacts/Release"
APP="$ARTEFACTS/Standalone/$PRODUCT.app"
AU="$ARTEFACTS/AU/$PRODUCT.component"
VST3="$ARTEFACTS/VST3/$PRODUCT.vst3"
for bundle in "$APP" "$AU" "$VST3"; do
  [[ -d "$bundle" ]] || { echo "Missing $bundle" >&2; exit 1; }
  binary="$(find "$bundle/Contents/MacOS" -type f -perm +111 | head -1)"
  minos="$(otool -l "$binary" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
  arch="$(lipo -archs "$binary")"
  [[ "$minos" == "$MIN_MACOS" && "$arch" == "arm64" ]] || { echo "$bundle: macOS $minos, $arch (want $MIN_MACOS, arm64)" >&2; exit 1; }
done

# 2. Stage and sign, outside the source tree: Dropbox (and macOS) tag files there with extended
# attributes, which pkgbuild would carry into the installer as stray "._" files. Copies leave the
# attributes behind. Each bundle is signed on its own (no --deep); the app gets the hardened
# runtime's microphone entitlement for singing takes.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/novox-release.XXXXXX")"
trap 'rm -r "$WORK"' EXIT
STAGE="$WORK/stage"
mkdir -p "$STAGE/app" "$STAGE/au" "$STAGE/vst3" "$WORK/pkgs" "$WORK/scripts-au" "$OUT"
copy() { ditto --norsrc --noextattr --noqtn "$1" "$2"; }
copy "$APP" "$STAGE/app/$PRODUCT.app"
copy "$AU" "$STAGE/au/$PRODUCT.component"
copy "$VST3" "$STAGE/vst3/$PRODUCT.vst3"
copy "$PACKAGING/scripts-au/postinstall" "$WORK/scripts-au/postinstall"
xattr -cr "$STAGE" "$WORK/scripts-au" 2>/dev/null || true
sign() {  # bundle [entitlements]
  if [[ -n "${NOVOX_SIGN_APP:-}" ]]; then
    codesign --force --options runtime --timestamp --sign "$NOVOX_SIGN_APP" ${2:+--entitlements "$2"} "$1"
  else
    codesign --force --sign - ${2:+--entitlements "$2"} "$1"
  fi
  codesign --verify --strict "$1"
}
sign "$STAGE/app/$PRODUCT.app" "$PACKAGING/app.entitlements"
sign "$STAGE/au/$PRODUCT.component"
sign "$STAGE/vst3/$PRODUCT.vst3"

# 3. One component package per bundle, never relocated: the installer must not "update" a copy it
# finds elsewhere (a build folder) instead of installing where it belongs. pkgbuild writes each
# package's description; its payload and bill of materials are then rebuilt without the AppleDouble
# entries pkgbuild makes from an attribute macOS will not let us remove (com.apple.provenance),
# owned by root:wheel as pkgbuild's are.
component() {  # name root install-location [scripts]
  local plist="$WORK/pkgs/$1.plist" raw="$WORK/pkgs/$1-raw.pkg" dir="$WORK/pkgs/$1"
  pkgbuild --analyze --root "$2" "$plist" >/dev/null
  local i=0
  while /usr/libexec/PlistBuddy -c "Print :$i" "$plist" >/dev/null 2>&1; do
    plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"
    plutil -replace "$i.BundleOverwriteAction" -string upgrade "$plist"
    i=$((i + 1))
  done
  # pkgbuild also prints "write: Permission denied" for that attribute; the payload is rebuilt anyway.
  pkgbuild --root "$2" --component-plist "$plist" --identifier "$IDENTIFIER.$1" --version "$VERSION" \
    --install-location "$3" ${4:+--scripts "$4"} "$raw" >/dev/null 2>"$WORK/pkgbuild.log" || { cat "$WORK/pkgbuild.log" >&2; exit 1; }
  pkgutil --expand "$raw" "$dir"
  (cd "$2" && find . | COPYFILE_DISABLE=1 cpio -o --format odc --owner 0:0 2>/dev/null) | gzip -9 > "$dir/Payload"
  mkbom "$2" "$WORK/pkgs/$1-full.bom"
  lsbom "$WORK/pkgs/$1-full.bom" | sed -E $'s#\t[0-9]+/[0-9]+#\t0/0#' > "$WORK/pkgs/$1-bom.txt"
  mv "$dir/Bom" "$WORK/pkgs/$1-pkgbuild.bom"
  mkbom -i "$WORK/pkgs/$1-bom.txt" "$dir/Bom"
  local files kb
  files="$(find "$2" | wc -l | tr -d ' ')"
  kb="$(du -sk "$2" | cut -f1)"
  sed -i '' -e "s/numberOfFiles=\"[0-9]*\"/numberOfFiles=\"$files\"/" -e "s/installKBytes=\"[0-9]*\"/installKBytes=\"$kb\"/" "$dir/PackageInfo"
  mkdir -p "$WORK/final"
  pkgutil --flatten "$dir" "$WORK/final/$1.pkg"
  if pkgutil --payload-files "$WORK/final/$1.pkg" | grep -q '/\._'; then echo "$1.pkg still has AppleDouble files" >&2; exit 1; fi
}
component app "$STAGE/app" /Applications
component au "$STAGE/au" /Library/Audio/Plug-Ins/Components "$WORK/scripts-au"
component vst3 "$STAGE/vst3" /Library/Audio/Plug-Ins/VST3

# 4. The installer, with the welcome, license and finish pages.
RESOURCES="$WORK/resources"
mkdir -p "$RESOURCES"
for page in welcome.html conclusion.html; do
  sed -e "s/@LABEL@/$LABEL/g" -e "s/@MIN_MACOS@/${MIN_MACOS%.0}/g" "$PACKAGING/$page" > "$RESOURCES/$page"
done
# The license page: the notices, then the AGPLv3 in full.
{ cat "$PACKAGING/license.txt"; printf '\n\n'; cat "$ROOT/plugin/LICENSE-AGPL-3.0.txt"; } > "$RESOURCES/license.txt"
sed -e "s/@LABEL@/$LABEL/g" -e "s/@VERSION@/$VERSION/g" -e "s/@MIN_MACOS@/$MIN_MACOS/g" \
  "$PACKAGING/distribution.xml" > "$WORK/distribution.xml"
PKG="$WORK/Novox-The-Robot-macOS.pkg"
productbuild --distribution "$WORK/distribution.xml" --resources "$RESOURCES" --package-path "$WORK/final" \
  ${NOVOX_SIGN_INSTALLER:+--sign "$NOVOX_SIGN_INSTALLER" --timestamp} "$PKG"

# 5. Notarize and staple (the ticket travels with the installer, so it opens offline too).
if [[ -n "${NOVOX_NOTARY_PROFILE:-}" ]]; then
  [[ -n "${NOVOX_SIGN_INSTALLER:-}" && -n "${NOVOX_SIGN_APP:-}" ]] || { echo "Notarizing needs both signing identities" >&2; exit 1; }
  xcrun notarytool submit "$PKG" --keychain-profile "$NOVOX_NOTARY_PROFILE" --wait
  xcrun stapler staple "$PKG"
  spctl --assess --type install -vv "$PKG"
else
  echo "== Not notarized (NOVOX_NOTARY_PROFILE unset): macOS will ask people to use Open Anyway"
fi

FINAL="$OUT/$(basename "$PKG")"
[[ -e "$FINAL" ]] && mv "$FINAL" "$FINAL.previous"
copy "$PKG" "$FINAL"
(cd "$OUT" && shasum -a 256 "$(basename "$FINAL")" > SHA256SUMS)
echo "== $FINAL ($(du -h "$FINAL" | cut -f1))"
