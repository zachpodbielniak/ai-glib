# config.mk - Build configuration for ai-glib
#
# Copyright (C) 2025
# SPDX-License-Identifier: AGPL-3.0-or-later

# Project information
PROJECT_NAME = ai-glib
VERSION_MAJOR = 1
VERSION_MINOR = 0
VERSION_MICRO = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_MICRO)

# Installation paths
PREFIX ?= /usr/local
INCLUDEDIR = $(PREFIX)/include
LIBDIR = $(PREFIX)/lib
PKGCONFIGDIR = $(LIBDIR)/pkgconfig

# Compiler settings
CC = gcc
CSTD = -std=gnu89
WARNINGS = -Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wshadow
CFLAGS_BASE = $(CSTD) $(WARNINGS) -fPIC

# pkg-config dependencies
PKG_DEPS = glib-2.0 gobject-2.0 gio-2.0 libsoup-3.0 json-glib-1.0

# Get flags from pkg-config
PKG_CFLAGS := $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS := $(shell pkg-config --libs $(PKG_DEPS))

# Build type configuration
ifdef DEBUG
    CFLAGS_OPT = -O0 -g3 -DDEBUG
else
    CFLAGS_OPT = -O2 -DNDEBUG
endif

ifdef ASAN
    CFLAGS_SAN = -fsanitize=address -fno-omit-frame-pointer
    LDFLAGS_SAN = -fsanitize=address
endif

ifdef UBSAN
    CFLAGS_SAN += -fsanitize=undefined
    LDFLAGS_SAN += -fsanitize=undefined
endif

# Combined flags
CFLAGS = $(CFLAGS_BASE) $(CFLAGS_OPT) $(CFLAGS_SAN) $(PKG_CFLAGS) \
         -DAI_GLIB_COMPILATION \
         -DAI_VERSION_MAJOR=$(VERSION_MAJOR) \
         -DAI_VERSION_MINOR=$(VERSION_MINOR) \
         -DAI_VERSION_MICRO=$(VERSION_MICRO) \
         -I$(SRCDIR) -Ibuild

LDFLAGS = $(LDFLAGS_SAN) $(PKG_LIBS)

# Directory structure
SRCDIR = src
BUILDDIR = build
OBJDIR = $(BUILDDIR)/obj
TESTDIR = tests
EXAMPLEDIR = examples
DOCSDIR = docs

# Library names
LIB_NAME = lib$(PROJECT_NAME)-1.0
LIB_SHARED = $(BUILDDIR)/$(LIB_NAME).so.$(VERSION)
LIB_SONAME = $(LIB_NAME).so.$(VERSION_MAJOR)
LIB_STATIC = $(BUILDDIR)/$(LIB_NAME).a

# GObject Introspection (optional)
GIR_NAMESPACE = AiGlib
GIR_VERSION = 1.0
GIR_FILE = $(BUILDDIR)/$(GIR_NAMESPACE)-$(GIR_VERSION).gir
TYPELIB_FILE = $(BUILDDIR)/$(GIR_NAMESPACE)-$(GIR_VERSION).typelib
