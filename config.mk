# config.mk - Build configuration for ai-glib
#
# Copyright (C) 2025
# SPDX-License-Identifier: AGPL-3.0-or-later

# Project information
PROJECT_NAME = ai-glib
VERSION_MAJOR = 0
VERSION_MINOR = 3
VERSION_MICRO = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)
PACKAGE_BUGREPORT = https://gitlab.com/zachpodbielniak/ai-glib/-/issues

# Installation paths
PREFIX ?= /usr/local
INCLUDEDIR ?= $(PREFIX)/include
LIBDIR ?= $(PREFIX)/lib
PKGCONFIGDIR = $(LIBDIR)/pkgconfig

# Compiler settings
CC = gcc
AR = ar
CSTD = -std=gnu89
WARNINGS = -Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wshadow
CFLAGS_BASE = $(CSTD) $(WARNINGS) -fPIC

# pkg-config dependencies
PKG_DEPS = glib-2.0 gobject-2.0 gio-2.0 libsoup-3.0 json-glib-1.0 libxml-2.0

# Get flags from pkg-config
PKG_CFLAGS := $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS := $(shell pkg-config --libs $(PKG_DEPS))

# yaml-glib (bundled under deps/yaml-glib, built as static lib).
# Always built with its own conventions; not affected by parent DEBUG.
# yaml-glib's build splits output by type (build/release/, build/debug/)
# and ships the archive with an API-version suffix. We pin to release
# since this static lib is only used at compile-time.
# ncursesw, for the `ai-tui` front-end only.
#
# Optional on purpose: the library itself has no terminal dependency, and a
# machine without ncurses should still build everything else. When it is
# missing, ai-tui is dropped from the binaries with a notice rather than
# failing the build.
NCURSES_DEPS = ncursesw
HAVE_NCURSES := $(shell pkg-config --exists $(NCURSES_DEPS) && echo 1 || echo 0)

ifeq ($(HAVE_NCURSES),1)
NCURSES_CFLAGS := $(shell pkg-config --cflags $(NCURSES_DEPS)) \
                  $(shell pkg-config --cflags gio-unix-2.0)
NCURSES_LIBS := $(shell pkg-config --libs $(NCURSES_DEPS)) \
                $(shell pkg-config --libs gio-unix-2.0)
endif

YAML_GLIB_DIR = deps/yaml-glib
YAML_GLIB_STATIC = $(YAML_GLIB_DIR)/build/release/libyaml-glib-1.0.a
YAML_GLIB_CFLAGS = -I$(YAML_GLIB_DIR)/src
YAML_GLIB_LIBS = $(YAML_GLIB_STATIC) $(shell pkg-config --libs yaml-0.1)

# ---- Build directories ----

BUILDDIR := build
DEBUG    ?= 0

ifeq ($(DEBUG),1)
    BUILD_TYPE := debug
    CFLAGS_OPT  = -O0 -g3 -DDEBUG
else
    BUILD_TYPE := release
    CFLAGS_OPT  = -O2 -DNDEBUG
endif

OUTDIR := $(BUILDDIR)/$(BUILD_TYPE)
OBJDIR := $(OUTDIR)/obj
BUILD_FLAGS_STAMP := $(OUTDIR)/.build-flags

# ---- Build options (opt-in toggles) ----

ASAN ?= 0
UBSAN ?= 0
GIR  ?= 0

ifeq ($(ASAN),1)
    CFLAGS_SAN = -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS_SAN = -fsanitize=address
endif

ifeq ($(UBSAN),1)
    CFLAGS_SAN += -fsanitize=undefined
    LDFLAGS_SAN += -fsanitize=undefined
endif

# Source and test directories
SRCDIR = src
TESTDIR = tests
EXAMPLEDIR = examples
BINDIR = bin
DOCSDIR = docs

# Combined flags. -I$(OUTDIR) picks up the per-build-type generated headers
# (config.h, ai-version.h) so a release build never compiles against the
# debug-tree's headers or vice versa.
CFLAGS = $(CFLAGS_BASE) $(CFLAGS_OPT) $(CFLAGS_SAN) $(PKG_CFLAGS) \
         $(YAML_GLIB_CFLAGS) \
         -DAI_GLIB_COMPILATION \
         -DAI_VERSION_MAJOR=$(VERSION_MAJOR) \
         -DAI_VERSION_MINOR=$(VERSION_MINOR) \
         -DAI_VERSION_MICRO=$(VERSION_MICRO) \
         -I$(SRCDIR) -I$(OUTDIR)

LDFLAGS = $(LDFLAGS_SAN) $(YAML_GLIB_LIBS) $(PKG_LIBS)

# Freeze the build-wide values before any target-specific additions (ncurses
# for ai-tui) are applied. These are what the incremental-build signature
# records.
BUILD_CONFIG_CFLAGS := $(CFLAGS)
BUILD_CONFIG_LDFLAGS := $(LDFLAGS)

# Library names
LIB_NAME = lib$(PROJECT_NAME)-1.0
LIB_SHARED = $(OUTDIR)/$(LIB_NAME).so.$(VERSION)
LIB_SONAME = $(LIB_NAME).so.$(VERSION_MAJOR)
LIB_STATIC = $(OUTDIR)/$(LIB_NAME).a

# GObject Introspection (opt-in: build with GIR=1)
GIR_SCANNER  = g-ir-scanner
GIR_COMPILER = g-ir-compiler
GIR_NAMESPACE = AiGlib
GIR_VERSION = 1.0
GIR_FILE = $(OUTDIR)/$(GIR_NAMESPACE)-$(GIR_VERSION).gir
TYPELIB_FILE = $(OUTDIR)/$(GIR_NAMESPACE)-$(GIR_VERSION).typelib
