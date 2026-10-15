# Wings TV app - build entry points (run inside the app build image).
# Adapted from Orbit Store's app/Makefile, itself from ps5-homebrew-ui /
# ps5-native-app-boilerplate (GPL-3.0-or-later).
#
#   docker build -t wings-app-build:0.1 wings/app
#   docker run --rm -v "$PWD:/work" -w /work/app wings-app-build:0.1 make ffpkg

SHELL := /bin/bash
.DEFAULT_GOAL := app

# The kit's build reads these from the environment.
APP_DEFINITIONS ?=
APP_INCLUDE_PATHS ?=
APP_STATIC_ARCHIVES ?=
APP_IMPORT_STUBS ?=
APP_RUNTIME_MODULES ?=
PS5_OPENGL_PREFIX ?=
OPENGL_SDK := .deps/ps5-opengl/current
override APP_DEFINITIONS := $(strip GL_GLEXT_PROTOTYPES=1 $(APP_DEFINITIONS))
# src/wings_core is the Pegasus adapter library (wings/core), copied in by
# tools/build-wings-wsl.sh; its headers are included as <wings/...>.
# src/platform/wings_art is the HTTPS cover art module (wings/art), staged in by
# wings/art/tools/stage-into-app.sh; its headers are also <wings/art/...>.
override APP_INCLUDE_PATHS := $(strip src src/wings_core/include src/platform/wings_art/include $(OPENGL_SDK)/include $(APP_INCLUDE_PATHS))
override APP_STATIC_ARCHIVES := $(strip .deps/ps5-opengl/libps5opengl-group.a $(APP_STATIC_ARCHIVES))
override APP_IMPORT_STUBS := $(strip $(OPENGL_SDK)/lib/libSceAgc.so \
	$(OPENGL_SDK)/lib/libSceAgcDriver.so $(APP_IMPORT_STUBS))
# WebP (the catalogue's artwork format) from the pinned PacBrew ports. The
# archives are named directly: the port's pkg-config output carries a
# compiler flag (-D_THREAD_SAFE) that the linker rejects. OpenSSL (libssl before
# libcrypto) is the cover art module's TLS.
PACBREW_PACKAGES ?=
PACBREW_INCLUDE_PATHS ?= include
PACBREW_STATIC_ARCHIVES ?= lib/libwebp.a lib/libsharpyuv.a lib/libssl.a lib/libcrypto.a
BUILD_JOBS ?= $(shell nproc 2>/dev/null || echo 2)
USE_CCACHE ?= 0
export BUILD_JOBS USE_CCACHE
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_IMPORT_STUBS APP_RUNTIME_MODULES
export PS5_OPENGL_PREFIX PACBREW_PACKAGES PACBREW_INCLUDE_PATHS PACBREW_STATIC_ARCHIVES

RUNTIME := runtime/libc.prx
RUNTIME_INPUTS := tools/rebuild-libc.sh tools/build-host-tools.sh tools/ninja-build.sh \
	$(wildcard tooling/native/*.cpp tooling/native/*.hpp) \
	$(wildcard tooling/native/runtime/*.txt)

.PHONY: app ffpkg ffpfsc packages deps opengl libc preview test fonts clean distclean help \
	helper helper-test

app: $(RUNTIME) opengl helper
	@printf '%s\n' '==> [app] Compiling, linking, signing and assembling the app folder'
	@bash tools/build.sh Folder

ffpkg: $(RUNTIME) opengl helper
	@printf '%s\n' '==> [ffpkg] Building the app folder and its UFS2 image'
	@bash tools/build.sh Ffpkg

ffpfsc: $(RUNTIME) opengl helper
	@printf '%s\n' '==> [ffpfsc] Building the app folder and its compressed image'
	@bash tools/build.sh Ffpfsc

packages: $(RUNTIME) opengl helper
	@bash tools/build.sh All

# The update helper (helper/): a payload the app sends to the console's ELF loader to place a
# downloaded Wings release in /data/homebrew. Built like Orbit Store's backend: the payload SDK's
# clang, static curl/OpenSSL from the PacBrew sysroot, cJSON compiled in, the CA bundle embedded.
# It lands in assets/, so build.sh ships it as /app0/assets/helper/wings-helper.elf.
HELPER_SDK := .deps/native/ps5-payload-sdk
HELPER_SYSROOT := .deps/pacbrew/v0.40.2/sysroot/user/homebrew
HELPER_CORE := helper/install.c helper/server.c third_party/cjson/cJSON.c
HELPER_HEADERS := $(wildcard helper/*.h) third_party/cjson/cJSON.h build/helper/ca.h
HELPER_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 \
	-Ihelper -Ithird_party/cjson -Ibuild/helper
HELPER_LIBS := -l:libcurl.a -l:libpsl.a -l:libssl.a -l:libcrypto.a -l:libzstd.a -l:libz.a -lpthread
HELPER_HOST_CC ?= clang-18
HELPER_ELF := assets/helper/wings-helper.elf

helper: $(HELPER_ELF)

build/helper/ca.h: assets/ca-bundle.crt
	@mkdir -p $(@D)
	@{ printf 'static const char wings_helper_ca[] =\n'; \
	   sed -e 's/\r$$//' -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $<; \
	   printf ';\n'; } > $@

$(HELPER_ELF): helper/main.c $(HELPER_CORE) $(HELPER_HEADERS) Makefile
	@printf '%s\n' '==> [helper] Building the update helper payload'
	@mkdir -p $(@D)
	LLVM_CONFIG=$${LLVM_CONFIG:-$$(command -v llvm-config-18)} $(HELPER_SDK)/bin/prospero-clang $(HELPER_CFLAGS) -Wno-unreachable-code-generic-assoc \
		-I$(HELPER_SYSROOT)/include helper/main.c $(HELPER_CORE) -o $@ \
		-L$(HELPER_SYSROOT)/lib $(HELPER_LIBS)
	LLVM_CONFIG=$${LLVM_CONFIG:-$$(command -v llvm-config-18)} $(HELPER_SDK)/bin/prospero-strip --strip-debug $@

# Host tests of the helper against a fake GitHub on 127.0.0.1 (needs libcurl and OpenSSL dev
# packages on the host).
build/helper/helper-test: $(HELPER_CORE) $(HELPER_HEADERS) tests/helper/helper_test.c Makefile
	@mkdir -p $(@D)
	$(HELPER_HOST_CC) $(HELPER_CFLAGS) -g -D_DEFAULT_SOURCE -DWINGS_HELPER_TEST \
		-fsanitize=address,undefined -fno-sanitize-recover=undefined \
		$(HELPER_CORE) tests/helper/helper_test.c -o $@ -lcurl -lssl -lcrypto -lpthread

helper-test: build/helper/helper-test
	@./build/helper/helper-test

deps:
	@bash tools/setup-test-dependencies.sh >/dev/null
	@bash tools/setup-native-dependencies.sh
	@bash tools/setup-pacbrew-dependencies.sh --environment
	@bash tools/prepare-opengl.sh

opengl:
	@bash tools/prepare-opengl.sh

libc:
	@bash tools/rebuild-libc.sh

$(RUNTIME): $(RUNTIME_INPUTS)
	@printf '%s\n' '==> [libc] Generating the clean-room loader shim'
	@bash tools/rebuild-libc.sh

# Pictures of every screen, rendered on this machine (build/preview/pictures).
preview:
	@bash tools/preview.sh build/preview/pictures all

test:
	@bash tools/build-tests.sh
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		build/tests/unit_tests --gtest_brief=1

fonts:
	@bash tools/bake-fonts.sh

clean:
	@rm -rf -- build dist
	@rm -f -- $(RUNTIME) $(HELPER_ELF)

distclean: clean
	@rm -rf -- .deps

help:
	@printf '%s\n' \
	  'make            Build the PS5 app folder in dist/PPSA99178' \
	  'make ffpkg      Also build dist/PPSA99178.ffpkg (ShadowMountPlus image)' \
	  'make preview    Render every screen on this machine to PNG' \
	  'make test       Host unit tests (sanitizers on)' \
	  'make helper     Build the update helper payload (assets/helper/wings-helper.elf)' \
	  'make helper-test  Host tests of the update helper' \
	  'make fonts      Rebake the Inter SDF fonts'
