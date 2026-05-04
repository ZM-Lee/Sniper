#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SKIP_BUILD="${SKIP_BUILD:-0}"
PROTOBUF_FORCE_SOURCE="${PROTOBUF_FORCE_SOURCE:-0}"
SUDO_CMD=""
APT_UPDATED=0
GST_INSPECT_BIN=""
GXIAPI_PATH=""

log() {
  printf "[setup] %s\n" "$*"
}

warn() {
  printf "[setup][warn] %s\n" "$*"
}

err() {
  printf "[setup][error] %s\n" "$*" >&2
}

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    err "missing command: $1"
    exit 1
  fi
}

version_ge() {
  local a="$1"
  local b="$2"
  [[ "$(printf "%s\n%s\n" "$b" "$a" | sort -V | tail -n1)" == "$a" ]]
}

is_debian_like() {
  [[ -f /etc/debian_version ]]
}

apt_install() {
  if [[ "$APT_UPDATED" == "0" ]]; then
    ${SUDO_CMD} apt-get update
    APT_UPDATED=1
  fi
  ${SUDO_CMD} apt-get install -y "$@"
}

install_base_packages() {
  log "install base packages"
  apt_install \
    build-essential cmake pkg-config curl ca-certificates tar \
    libopencv-dev libyaml-cpp-dev \
    libprotobuf-dev protobuf-compiler \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    libmosquitto-dev mosquitto mosquitto-clients
}

find_gxiapi_path() {
  local p
  for p in \
    /lib/libgxiapi.so \
    /usr/lib/libgxiapi.so \
    /usr/local/lib/libgxiapi.so; do
    if [[ -e "$p" ]]; then
      printf "%s\n" "$p"
      return 0
    fi
  done

  local line
  while IFS= read -r line; do
    case "$line" in
      *libgxiapi.so*)
        printf "%s\n" "${line##*=> }"
        return 0
        ;;
    esac
  done < <(/usr/sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null || true)

  return 1
}

handle_conda_protoc() {
  local conda_protoc="${HOME}/anaconda3/bin/protoc"
  local conda_protoc_backup="${HOME}/anaconda3/bin/protoc_"

  if [[ ! -e "$conda_protoc" ]]; then
    return
  fi

  if [[ -e "$conda_protoc_backup" ]]; then
    warn "${conda_protoc_backup} already exists, skip renaming ${conda_protoc}"
    return
  fi

  mv "$conda_protoc" "$conda_protoc_backup"
  log "renamed ${conda_protoc} -> ${conda_protoc_backup}"
}

install_protobuf_from_source() {
  local ver="3.19.3"
  local workdir
  workdir="$(mktemp -d)"

  log "install protobuf ${ver} from source"
  apt_install autoconf automake libtool make g++ unzip

  pushd "$workdir" >/dev/null
  curl -fL -o "protobuf-v${ver}.tar.gz" \
    "https://github.com/protocolbuffers/protobuf/archive/refs/tags/v${ver}.tar.gz"
  tar -xf "protobuf-v${ver}.tar.gz"

  pushd "protobuf-${ver}" >/dev/null
  cmake -S cmake -B build -Dprotobuf_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j"$(nproc)"
  ${SUDO_CMD} cmake --install build
  popd >/dev/null

  popd >/dev/null
  rm -rf "$workdir"
  ${SUDO_CMD} ldconfig
}

ensure_protobuf_version() {
  local required="3.19.3"
  local current=""

  if command -v protoc >/dev/null 2>&1; then
    current="$(protoc --version | awk '{print $2}')"
  fi

  if [[ "$PROTOBUF_FORCE_SOURCE" == "1" ]]; then
    log "protobuf source install forced (PROTOBUF_FORCE_SOURCE=1)"
    install_protobuf_from_source
    return
  fi

  if [[ -z "$current" ]]; then
    warn "protoc not found, will install protobuf 3.19.3 from source"
    install_protobuf_from_source
    return
  fi

  if [[ "$current" != "$required" ]]; then
    warn "protoc version ${current} != ${required}, will install protobuf 3.19.3 from source"
    install_protobuf_from_source
  else
    log "protobuf version ok: ${current}"
  fi
}

ensure_gstreamer_element() {
  local element="$1"
  local package="$2"

  if "$GST_INSPECT_BIN" "$element" >/dev/null 2>&1; then
    log "gstreamer ${element} found"
    return
  fi

  warn "${element} not found, installing ${package}"
  apt_install "$package"

  if ! "$GST_INSPECT_BIN" "$element" >/dev/null 2>&1; then
    err "${element} still missing after installing ${package}"
    exit 1
  fi
}

ensure_encoder_plugins() {
  ensure_gstreamer_element x264enc gstreamer1.0-plugins-ugly
  ensure_gstreamer_element h264parse gstreamer1.0-plugins-bad
}

ensure_gxiapi() {
  local detected
  detected="$(find_gxiapi_path || true)"

  if [[ -n "$detected" ]]; then
    GXIAPI_PATH="$detected"
    log "libgxiapi.so found: ${GXIAPI_PATH}"
    return
  fi

  err "libgxiapi.so not found. Please install Daheng Galaxy SDK manually, then rerun."
  exit 1
}

show_summary() {
  log "version summary"
  echo "OS: $(lsb_release -ds 2>/dev/null || awk -F= '/^PRETTY_NAME=/{gsub(/\"/,"",$2);print $2}' /etc/os-release)"
  echo "CMake: $(cmake --version | awk 'NR==1{print $3}')"
  echo "G++: $(g++ --version | awk 'NR==1{print $NF}')"
  echo "OpenCV: $(pkg-config --modversion opencv4 2>/dev/null || echo N/A)"
  echo "yaml-cpp: $(pkg-config --modversion yaml-cpp 2>/dev/null || echo N/A)"
  echo "Protobuf: $(protoc --version 2>/dev/null | awk '{print $2}' || echo N/A)"
  echo "GStreamer: $(pkg-config --modversion gstreamer-1.0 2>/dev/null || echo N/A)"
  echo "Mosquitto: $(mosquitto -h 2>&1 | awk 'NR==1{print $3}' || echo N/A)"
  echo "gxiapi: ${GXIAPI_PATH:-N/A}"
}

main() {
  if ! is_debian_like; then
    err "this script currently supports Debian/Ubuntu only"
    exit 1
  fi

  if command -v sudo >/dev/null 2>&1; then
    SUDO_CMD="sudo"
  elif [[ "$(id -u)" == "0" ]]; then
    SUDO_CMD=""
  else
    err "need sudo or run as root"
    exit 1
  fi

  if [[ -n "${CONDA_PREFIX:-}" ]]; then
    warn "conda environment detected: ${CONDA_PREFIX}"
    warn "if CMake finds wrong libs, run: conda deactivate"
  fi

  handle_conda_protoc

  if [[ -x /usr/bin/gst-inspect-1.0 ]]; then
    GST_INSPECT_BIN="/usr/bin/gst-inspect-1.0"
  else
    GST_INSPECT_BIN="$(command -v gst-inspect-1.0 || true)"
  fi
  if [[ -z "$GST_INSPECT_BIN" ]]; then
    err "gst-inspect-1.0 not found"
    exit 1
  fi

  install_base_packages
  ensure_protobuf_version
  ensure_encoder_plugins
  ensure_gxiapi

  if [[ "$SKIP_BUILD" != "1" ]]; then
    log "configure and build project"
    cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$REPO_ROOT/build" -j"$(nproc)"
  else
    log "skip build due to SKIP_BUILD=1"
  fi

  show_summary
  log "setup completed"
}

main "$@"
