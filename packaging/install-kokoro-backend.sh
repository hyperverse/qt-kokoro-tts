#!/usr/bin/env bash
# Fetch the Kokoro ONNX model and the `koko` server binary into
# ~/.local/share/kokoro, which is where the systemd user unit expects them.
set -euo pipefail

PREFIX="${KOKORO_HOME:-$HOME/.local/share/kokoro}"
KOKOROS_TAG="${KOKOROS_TAG:-b17}"
KOKOROS_URL="https://github.com/lemonade-sdk/Kokoros/releases/download/${KOKOROS_TAG}/kokoros-linux-x86_64.tar.gz"
MODEL_URL="https://huggingface.co/mikkoph/kokoro-onnx/resolve/main/kokoro-v1.0.onnx"
VOICES_URL="https://huggingface.co/mikkoph/kokoro-onnx/resolve/main/voices-v1.0.bin"

mkdir -p "$PREFIX"
cd "$PREFIX"

fetch() { # url, destination
    if [ -s "$2" ]; then
        echo "have $2"
    else
        echo "downloading $2"
        curl -fL --progress-bar -o "$2" "$1"
    fi
}

fetch "$MODEL_URL"   kokoro-v1.0.onnx
fetch "$VOICES_URL"  voices-v1.0.bin
fetch "$KOKOROS_URL" kokoros.tar.gz

tar xzf kokoros.tar.gz
test -x linux-x86_64/koko

echo
echo "installed into $PREFIX"
echo "start the server with:"
echo "  systemctl --user enable --now kokoro-server.service"
