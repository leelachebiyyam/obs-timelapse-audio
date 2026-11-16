#!/bin/zsh
#
# Build obs-timelapse against the OBS Studio that is already installed on this
# Mac, instead of downloading the SDK archives pinned in buildspec.json.
#
# The plugin is compiled against the exact libobs/libav* versions that the app
# loads, so it always matches the running OBS and never duplicates FFmpeg/Qt.
#
# Required environment:
#   OBS_SOURCE  extracted obs-studio source tree for the same release as OBS_APP
#   OBS_DEPS    obs-deps prefix holding the FFmpeg headers that OBS bundles
#   OBS_QT6     obs-deps Qt6 prefix holding the Qt headers that OBS bundles
#
# Optional environment:
#   OBS_APP     installed application (default /Applications/OBS.app)
#   OUT_DIR     build output directory (default ./build_local)
#   INSTALL     copy the finished bundle into the user plugin directory
set -euo pipefail

repo_dir=${0:A:h:h}
obs_app=${OBS_APP:-/Applications/OBS.app}
out_dir=${OUT_DIR:-${repo_dir}/build_local}
install=${INSTALL:-0}

die() {
	print -u2 "error: $1"
	exit 1
}

[[ -n ${OBS_SOURCE:-} ]] || die "set OBS_SOURCE to an extracted obs-studio source tree"
[[ -n ${OBS_DEPS:-} ]] || die "set OBS_DEPS to the obs-deps prefix with FFmpeg headers"
[[ -n ${OBS_QT6:-} ]] || die "set OBS_QT6 to the obs-deps Qt6 prefix"
[[ -d ${obs_app}/Contents/Frameworks ]] || die "no OBS frameworks in ${obs_app}"

version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' \
	"${repo_dir}/buildspec.json")
bundle_name=obs-timelapse
bundle_dir=${out_dir}/${bundle_name}.plugin

generated=${out_dir}/generated
resource_dir=${bundle_dir}/Contents/Resources
mkdir -p "${generated}" "${bundle_dir}/Contents/MacOS" "${resource_dir}"

cat >"${generated}/obsconfig.h" <<'EOF'
#pragma once
#define OBS_DATA_PATH "Resources"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
EOF

sed -e "s/@CMAKE_PROJECT_NAME@/${bundle_name}/g" -e "s/@CMAKE_PROJECT_VERSION@/${version}/g" \
	"${repo_dir}/src/plugin-support.c.in" >"${generated}/plugin-support.c"

cat >"${bundle_dir}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleExecutable</key><string>${bundle_name}</string>
	<key>CFBundleIdentifier</key><string>com.openfu.obs-timelapse</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>${bundle_name}</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleShortVersionString</key><string>${version}</string>
	<key>CFBundleVersion</key><string>${version}</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

cp -R "${repo_dir}/data/." "${resource_dir}/"

frameworks=${obs_app}/Contents/Frameworks

include_args=(
	-I"${repo_dir}/src"
	-I"${generated}"
	-I"${OBS_SOURCE}/libobs"
	-I"${OBS_SOURCE}/frontend/api"
	-I"${OBS_DEPS}/include"
	-I"${OBS_QT6}/lib/QtCore.framework/Headers"
	-I"${OBS_QT6}/lib/QtGui.framework/Headers"
	-I"${OBS_QT6}/lib/QtWidgets.framework/Headers"
	-F"${OBS_QT6}/lib"
	-F"${frameworks}"
)

# plugin-support.c is plain C: `template` is a C++ keyword.
clang -std=c11 -O2 -fPIC -mmacosx-version-min=13.0 -c "${generated}/plugin-support.c" \
	-o "${generated}/plugin-support.o" "${include_args[@]}"

objects=("${generated}/plugin-support.o")
for source in "${repo_dir}"/src/*.cpp; do
	object="${generated}/${source:t:r}.o"
	clang++ -std=c++17 -O2 -fPIC -mmacosx-version-min=13.0 -Wall -Wextra -Wno-unused-parameter \
		-c "${source}" -o "${object}" "${include_args[@]}"
	objects+=("${object}")
done

print "Linking ${version} against ${obs_app}"
clang++ -bundle -mmacosx-version-min=13.0 -o "${bundle_dir}/Contents/MacOS/${bundle_name}" \
	"${objects[@]}" \
	-F"${frameworks}" -framework QtCore -framework QtGui -framework QtWidgets -framework libobs \
	"${frameworks}/obs-frontend-api.dylib" \
	"${frameworks}/libavcodec.dylib" \
	"${frameworks}/libavfilter.dylib" \
	"${frameworks}/libavformat.dylib" \
	"${frameworks}/libavutil.dylib" \
	"${frameworks}/libswresample.dylib" \
	"${frameworks}/libswscale.dylib"

codesign --force --sign - "${bundle_dir}" 2>/dev/null || print "warning: ad-hoc signing failed"

print "Built ${bundle_dir}"

if [[ ${install} == 1 ]]; then
	target="${HOME}/Library/Application Support/obs-studio/plugins/${bundle_name}.plugin"
	rm -rf "${target}"
	mkdir -p "${target:h}"
	cp -R "${bundle_dir}" "${target}"
	codesign --force --sign - "${target}" 2>/dev/null || print "warning: ad-hoc signing failed"
	print "Installed ${target}"
fi
