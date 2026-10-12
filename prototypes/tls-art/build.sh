#!/usr/bin/env bash
# Build tls_art_test inside the container (see run.sh). Compiles the app's own
# image decoder (read-only, from vendor/) next to the prototype client.
set -euo pipefail
root=${WB_ROOT:-/work}
app=$root/vendor/orbit-src/x/src/orbit-store-1.1.0/app
out=${OUT:-/tmp/tls-art-build}
mkdir -p "$out"
cd "$root/wings/prototypes/tls-art"
clang -std=c11 -O2 -w -I"$app/src/third_party/stb" -c "$app/src/third_party/stb/image.c" -o "$out/stb.o"
clang++-18 -std=c++20 -O2 -Wall -Wextra -I"$app/src" -c "$app/src/orbit/image.cpp" -o "$out/image.o"
clang++-18 -std=c++20 -O2 -Wall -Wextra -I"$app/src" -c tls_client.cpp -o "$out/tls_client.o"
clang++-18 -std=c++20 -O2 -Wall -Wextra -I"$app/src" -c doh.cpp -o "$out/doh.o"
clang++-18 -std=c++20 -O2 -Wall -Wextra -I"$app/src" -c main.cpp -o "$out/main.o"
clang++-18 "$out"/*.o -o "$out/tls_art_test" -lssl -lcrypto -lwebp -lsharpyuv -lpthread -lm
echo "built $out/tls_art_test"
