# Makefile - Main build file for ai-glib
#
# Copyright (C) 2025
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage:
#   make                 - Build shared and static libraries
#   make DEBUG=1         - Debug build
#   make test            - Run tests
#   make install         - Install to PREFIX
#   make help            - Show all targets

include config.mk

# Public headers (to be installed)
PUBLIC_HEADERS = \
	$(SRCDIR)/ai-glib.h \
	$(SRCDIR)/ai-types.h \
	$(SRCDIR)/core/ai-error.h \
	$(SRCDIR)/core/ai-enums.h \
	$(SRCDIR)/core/ai-config.h \
	$(SRCDIR)/core/ai-provider.h \
	$(SRCDIR)/core/ai-streamable.h \
	$(SRCDIR)/core/ai-image-generator.h \
	$(SRCDIR)/core/ai-client.h \
	$(SRCDIR)/core/ai-cli-client.h \
	$(SRCDIR)/core/ai-prompt-scorer.h \
	$(SRCDIR)/model/ai-usage.h \
	$(SRCDIR)/model/ai-content-block.h \
	$(SRCDIR)/model/ai-text-content.h \
	$(SRCDIR)/model/ai-tool.h \
	$(SRCDIR)/model/ai-tool-use.h \
	$(SRCDIR)/model/ai-tool-result.h \
	$(SRCDIR)/model/ai-message.h \
	$(SRCDIR)/model/ai-response.h \
	$(SRCDIR)/model/ai-image-request.h \
	$(SRCDIR)/model/ai-generated-image.h \
	$(SRCDIR)/model/ai-image-response.h \
	$(SRCDIR)/providers/ai-claude-client.h \
	$(SRCDIR)/providers/ai-openai-client.h \
	$(SRCDIR)/providers/ai-grok-client.h \
	$(SRCDIR)/providers/ai-gemini-client.h \
	$(SRCDIR)/providers/ai-ollama-client.h \
	$(SRCDIR)/providers/ai-claude-code-client.h \
	$(SRCDIR)/providers/ai-opencode-client.h \
	$(SRCDIR)/convenience/ai-simple.h \
	$(SRCDIR)/convenience/ai-search-provider.h \
	$(SRCDIR)/convenience/ai-bing-search.h \
	$(SRCDIR)/convenience/ai-brave-search.h \
	$(SRCDIR)/convenience/ai-tool-executor.h

# Library source files
LIB_SOURCES = \
	$(SRCDIR)/core/ai-error.c \
	$(SRCDIR)/core/ai-enums.c \
	$(SRCDIR)/core/ai-config.c \
	$(SRCDIR)/core/ai-provider.c \
	$(SRCDIR)/core/ai-streamable.c \
	$(SRCDIR)/core/ai-image-generator.c \
	$(SRCDIR)/core/ai-client.c \
	$(SRCDIR)/core/ai-cli-client.c \
	$(SRCDIR)/core/ai-prompt-scorer.c \
	$(SRCDIR)/model/ai-usage.c \
	$(SRCDIR)/model/ai-content-block.c \
	$(SRCDIR)/model/ai-text-content.c \
	$(SRCDIR)/model/ai-tool.c \
	$(SRCDIR)/model/ai-tool-use.c \
	$(SRCDIR)/model/ai-tool-result.c \
	$(SRCDIR)/model/ai-message.c \
	$(SRCDIR)/model/ai-response.c \
	$(SRCDIR)/model/ai-image-request.c \
	$(SRCDIR)/model/ai-generated-image.c \
	$(SRCDIR)/model/ai-image-response.c \
	$(SRCDIR)/providers/ai-claude-client.c \
	$(SRCDIR)/providers/ai-openai-client.c \
	$(SRCDIR)/providers/ai-grok-client.c \
	$(SRCDIR)/providers/ai-gemini-client.c \
	$(SRCDIR)/providers/ai-ollama-client.c \
	$(SRCDIR)/providers/ai-claude-code-client.c \
	$(SRCDIR)/providers/ai-opencode-client.c \
	$(SRCDIR)/convenience/ai-simple.c \
	$(SRCDIR)/convenience/ai-search-provider.c \
	$(SRCDIR)/convenience/ai-bing-search.c \
	$(SRCDIR)/convenience/ai-brave-search.c \
	$(SRCDIR)/convenience/ai-tool-executor.c

# Object files
LIB_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_SOURCES))

# Test files
TEST_SOURCES = $(wildcard $(TESTDIR)/test-*.c)
TEST_BINARIES = $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/tests/%,$(TEST_SOURCES))

# Example files
EXAMPLE_SOURCES = $(wildcard $(EXAMPLEDIR)/*.c)
EXAMPLE_BINARIES = $(patsubst $(EXAMPLEDIR)/%.c,$(BUILDDIR)/examples/%,$(EXAMPLE_SOURCES))

# Include common rules
include rules.mk

# Build bundled yaml-glib static library
$(YAML_GLIB_STATIC):
	@echo "Building yaml-glib..."
	@mkdir -p $(YAML_GLIB_DIR)/build
	$(MAKE) -C $(YAML_GLIB_DIR) lib-static

# Default target
.PHONY: all
all: $(BUILDDIR)/config.h $(BUILDDIR)/ai-version.h shared static $(PROJECT_NAME)-1.0.pc

# Generate config.h from template
$(BUILDDIR)/config.h: $(SRCDIR)/config.h.in | $(BUILDDIR)
	@echo "Generating config.h..."
	@sed -e 's/@VERSION_MAJOR@/$(VERSION_MAJOR)/g' \
	     -e 's/@VERSION_MINOR@/$(VERSION_MINOR)/g' \
	     -e 's/@VERSION_MICRO@/$(VERSION_MICRO)/g' \
	     -e 's/@PACKAGE_NAME@/$(PROJECT_NAME)/g' \
	     $< > $@

# Generate ai-version.h from template
$(BUILDDIR)/ai-version.h: $(SRCDIR)/ai-version.h.in | $(BUILDDIR)
	@echo "Generating ai-version.h..."
	@sed -e 's/@VERSION_MAJOR@/$(VERSION_MAJOR)/g' \
	     -e 's/@VERSION_MINOR@/$(VERSION_MINOR)/g' \
	     -e 's/@VERSION_MICRO@/$(VERSION_MICRO)/g' \
	     $< > $@

# Shared library
.PHONY: shared
shared: $(LIB_SHARED)

$(LIB_SHARED): $(LIB_OBJECTS) | $(BUILDDIR)
	@echo "Linking shared library..."
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -o $@ $(LIB_OBJECTS) $(LDFLAGS)
	@cd $(BUILDDIR) && ln -sf $(notdir $(LIB_SHARED)) $(LIB_SONAME) 2>/dev/null || true
	@cd $(BUILDDIR) && ln -sf $(LIB_SONAME) $(LIB_NAME).so 2>/dev/null || true

# Static library
.PHONY: static
static: $(LIB_STATIC)

$(LIB_STATIC): $(LIB_OBJECTS) | $(BUILDDIR)
	@echo "Creating static library..."
	$(AR) rcs $@ $(LIB_OBJECTS)

# Ensure config.h, ai-version.h, and yaml-glib exist before compiling
$(LIB_OBJECTS): $(BUILDDIR)/config.h $(BUILDDIR)/ai-version.h $(YAML_GLIB_STATIC)

# pkg-config file
$(PROJECT_NAME)-1.0.pc: $(PROJECT_NAME)-1.0.pc.in
	@echo "Generating pkg-config file..."
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     $< > $@

# Tests
.PHONY: test
test: $(TEST_BINARIES)
	@echo "Running tests..."
	@for test in $(TEST_BINARIES); do \
		echo "Running $$test..."; \
		$$test || exit 1; \
	done
	@echo "All tests passed!"

.PHONY: test-verbose
test-verbose: $(TEST_BINARIES)
	@echo "Running tests (verbose)..."
	@for test in $(TEST_BINARIES); do \
		echo "Running $$test..."; \
		G_TEST_VERBOSE=1 $$test || exit 1; \
	done
	@echo "All tests passed!"

# Examples
.PHONY: examples
examples: $(EXAMPLE_BINARIES)

# GObject introspection
.PHONY: gir
gir: $(TYPELIB_FILE)

$(GIR_FILE): $(LIB_SHARED) $(PUBLIC_HEADERS)
	@echo "Generating GObject introspection data..."
	g-ir-scanner --namespace=$(GIR_NAMESPACE) \
		--nsversion=$(GIR_VERSION) \
		--warn-all \
		--include=GLib-2.0 \
		--include=GObject-2.0 \
		--include=Gio-2.0 \
		--include=Soup-3.0 \
		--include=Json-1.0 \
		--pkg=glib-2.0 \
		--pkg=gobject-2.0 \
		--pkg=gio-2.0 \
		--pkg=libsoup-3.0 \
		--pkg=json-glib-1.0 \
		--library=$(LIB_NAME) \
		--library-path=$(BUILDDIR) \
		-I$(SRCDIR) \
		-Ibuild \
		--output=$@ \
		$(PUBLIC_HEADERS) $(LIB_SOURCES)

$(TYPELIB_FILE): $(GIR_FILE)
	g-ir-compiler $< -o $@

# Installation
.PHONY: install
install: all
	@echo "Installing to $(PREFIX)..."
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/core
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/model
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/providers
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/convenience
	install -d $(DESTDIR)$(PKGCONFIGDIR)
	install -m 644 $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(notdir $(LIB_SHARED)) $(LIB_SONAME)
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SONAME) $(LIB_NAME).so
	install -m 644 $(SRCDIR)/ai-glib.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/
	install -m 644 $(SRCDIR)/ai-types.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/
	install -m 644 $(BUILDDIR)/ai-version.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/
	install -m 644 $(SRCDIR)/core/*.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/core/
	install -m 644 $(SRCDIR)/model/*.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/model/
	install -m 644 $(SRCDIR)/providers/*.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/providers/
	install -m 644 $(SRCDIR)/convenience/*.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/convenience/
	install -m 644 $(PROJECT_NAME)-1.0.pc $(DESTDIR)$(PKGCONFIGDIR)/
	@echo "Installation complete!"

.PHONY: uninstall
uninstall:
	@echo "Uninstalling from $(PREFIX)..."
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME).so*
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME).a
	rm -rf $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/$(PROJECT_NAME)-1.0.pc
	@echo "Uninstallation complete!"

# Print variables (for debugging)
.PHONY: vars
vars:
	@echo "PROJECT_NAME  = $(PROJECT_NAME)"
	@echo "VERSION       = $(VERSION)"
	@echo "CC            = $(CC)"
	@echo "CFLAGS        = $(CFLAGS)"
	@echo "LDFLAGS       = $(LDFLAGS)"
	@echo "PREFIX        = $(PREFIX)"
	@echo "LIB_SOURCES   = $(LIB_SOURCES)"
	@echo "LIB_OBJECTS   = $(LIB_OBJECTS)"
