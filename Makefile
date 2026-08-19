# Makefile - Main build file for ai-glib
#
# Copyright (C) 2025
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# Usage:
#   make                 - Release build to build/release/
#   make DEBUG=1         - Debug build to build/debug/
#   make GIR=1           - Also build GObject introspection (.gir/.typelib)
#   make test            - Run tests
#   make install         - Install to PREFIX
#   make help            - Show all targets

# When clean and build targets appear together (e.g. make clean all -j),
# serialize them via sub-make so clean finishes before the build starts.
ifneq ($(filter clean clean-all distclean,$(MAKECMDGOALS)),)
ifneq ($(filter-out clean clean-all distclean,$(MAKECMDGOALS)),)

.PHONY: $(MAKECMDGOALS) __serialize
$(MAKECMDGOALS): __serialize ;
__serialize:
	$(MAKE) --no-print-directory $(filter clean clean-all distclean,$(MAKECMDGOALS))
	$(MAKE) --no-print-directory $(filter-out clean clean-all distclean,$(MAKECMDGOALS))

__MIXED := 1
endif
endif

ifndef __MIXED

include config.mk

# Make `make` (no args) build everything. Without this, the first target
# defined by include rules.mk ($(OUTDIR):) would become the default goal.
.DEFAULT_GOAL := all

# Public headers (to be installed)
PUBLIC_HEADERS = \
	$(SRCDIR)/ai-glib.h \
	$(SRCDIR)/ai-types.h \
	$(SRCDIR)/core/ai-error.h \
	$(SRCDIR)/core/ai-enums.h \
	$(SRCDIR)/core/ai-config.h \
	$(SRCDIR)/core/ai-provider.h \
	$(SRCDIR)/core/ai-streamable.h \
	$(SRCDIR)/core/ai-event.h \
	$(SRCDIR)/core/ai-event-source.h \
	$(SRCDIR)/core/ai-image-capabilities.h \
	$(SRCDIR)/core/ai-image-generator.h \
	$(SRCDIR)/core/ai-client.h \
	$(SRCDIR)/core/ai-cli-client.h \
	$(SRCDIR)/core/ai-tool-endpoint.h \
	$(SRCDIR)/core/ai-tool-endpoint-consumer.h \
	$(SRCDIR)/core/ai-prompt-scorer.h \
	$(SRCDIR)/view/ai-style.h \
	$(SRCDIR)/view/ai-tool-call.h \
	$(SRCDIR)/view/ai-tool-style.h \
	$(SRCDIR)/view/ai-view-block.h \
	$(SRCDIR)/view/ai-view-blocks.h \
	$(SRCDIR)/view/ai-view-tool-block.h \
	$(SRCDIR)/view/ai-transcript.h \
	$(SRCDIR)/view/ai-conversation.h \
	$(SRCDIR)/model/ai-usage.h \
	$(SRCDIR)/model/ai-todo.h \
	$(SRCDIR)/model/ai-content-block.h \
	$(SRCDIR)/model/ai-text-content.h \
	$(SRCDIR)/model/ai-image-content.h \
	$(SRCDIR)/model/ai-tool.h \
	$(SRCDIR)/model/ai-tool-use.h \
	$(SRCDIR)/model/ai-tool-result.h \
	$(SRCDIR)/model/ai-message.h \
	$(SRCDIR)/model/ai-response.h \
	$(SRCDIR)/model/ai-image.h \
	$(SRCDIR)/model/ai-image-request.h \
	$(SRCDIR)/model/ai-generated-image.h \
	$(SRCDIR)/model/ai-image-response.h \
	$(SRCDIR)/providers/ai-claude-client.h \
	$(SRCDIR)/providers/ai-openai-client.h \
	$(SRCDIR)/providers/ai-grok-client.h \
	$(SRCDIR)/providers/ai-gemini-client.h \
	$(SRCDIR)/providers/ai-ollama-client.h \
	$(SRCDIR)/providers/ai-claude-code-client.h \
	$(SRCDIR)/providers/ai-claude-tmux-client.h \
	$(SRCDIR)/providers/ai-opencode-client.h \
	$(SRCDIR)/providers/ai-grok-build-client.h \
	$(SRCDIR)/convenience/ai-simple.h \
	$(SRCDIR)/convenience/ai-search-provider.h \
	$(SRCDIR)/convenience/ai-search-result.h \
	$(SRCDIR)/convenience/ai-search-options.h \
	$(SRCDIR)/convenience/ai-bing-search.h \
	$(SRCDIR)/convenience/ai-brave-search.h \
	$(SRCDIR)/convenience/ai-duckduckgo-search.h \
	$(SRCDIR)/convenience/ai-tool-executor.h \
	$(SRCDIR)/convenience/ai-provider-factory.h \
	$(SRCDIR)/agent/ai-agent-enums.h \
	$(SRCDIR)/agent/ai-budget.h \
	$(SRCDIR)/agent/ai-price-table.h \
	$(SRCDIR)/agent/ai-agent-worker.h \
	$(SRCDIR)/agent/ai-agent-host.h \
	$(SRCDIR)/agent/ai-agent-store.h \
	$(SRCDIR)/agent/ai-agent-isolation.h \
	$(SRCDIR)/agent/ai-agent.h \
	$(SRCDIR)/agent/ai-brigade.h \
	$(SRCDIR)/agent/ai-local-worker.h \
	$(SRCDIR)/agent/ai-mock-provider.h \
	$(SRCDIR)/harness/ai-resource.h \
	$(SRCDIR)/harness/ai-resource-registry.h \
	$(SRCDIR)/harness/ai-mention.h \
	$(SRCDIR)/harness/ai-command.h \
	$(SRCDIR)/harness/ai-completion.h

# Library source files
LIB_SOURCES = \
	$(SRCDIR)/core/ai-error.c \
	$(SRCDIR)/core/ai-http-error.c \
	$(SRCDIR)/core/ai-enums.c \
	$(SRCDIR)/core/ai-config.c \
	$(SRCDIR)/core/ai-provider.c \
	$(SRCDIR)/core/ai-streamable.c \
	$(SRCDIR)/core/ai-event.c \
	$(SRCDIR)/core/ai-event-source.c \
	$(SRCDIR)/core/ai-image-capabilities.c \
	$(SRCDIR)/core/ai-image-generator.c \
	$(SRCDIR)/core/ai-client.c \
	$(SRCDIR)/core/ai-subprocess-util.c \
	$(SRCDIR)/core/ai-cli-client.c \
	$(SRCDIR)/core/ai-tool-endpoint.c \
	$(SRCDIR)/core/ai-tool-endpoint-consumer.c \
	$(SRCDIR)/core/ai-prompt-scorer.c \
	$(SRCDIR)/view/ai-style.c \
	$(SRCDIR)/view/ai-tool-call.c \
	$(SRCDIR)/view/ai-tool-style.c \
	$(SRCDIR)/view/ai-view-block.c \
	$(SRCDIR)/view/ai-view-blocks.c \
	$(SRCDIR)/view/ai-view-tool-block.c \
	$(SRCDIR)/view/ai-transcript.c \
	$(SRCDIR)/view/ai-conversation.c \
	$(SRCDIR)/model/ai-usage.c \
	$(SRCDIR)/model/ai-todo.c \
	$(SRCDIR)/model/ai-content-block.c \
	$(SRCDIR)/model/ai-text-content.c \
	$(SRCDIR)/model/ai-image-content.c \
	$(SRCDIR)/model/ai-tool.c \
	$(SRCDIR)/model/ai-tool-use.c \
	$(SRCDIR)/model/ai-tool-result.c \
	$(SRCDIR)/model/ai-message.c \
	$(SRCDIR)/model/ai-response.c \
	$(SRCDIR)/model/ai-image.c \
	$(SRCDIR)/model/ai-image-request.c \
	$(SRCDIR)/model/ai-generated-image.c \
	$(SRCDIR)/model/ai-image-response.c \
	$(SRCDIR)/providers/ai-image-shared.c \
	$(SRCDIR)/providers/ai-openai-shared.c \
	$(SRCDIR)/providers/ai-claude-launch.c \
	$(SRCDIR)/providers/ai-claude-client.c \
	$(SRCDIR)/providers/ai-openai-client.c \
	$(SRCDIR)/providers/ai-grok-client.c \
	$(SRCDIR)/providers/ai-gemini-client.c \
	$(SRCDIR)/providers/ai-ollama-client.c \
	$(SRCDIR)/providers/ai-claude-code-client.c \
	$(SRCDIR)/providers/ai-claude-tmux-client.c \
	$(SRCDIR)/providers/ai-opencode-client.c \
	$(SRCDIR)/providers/ai-grok-build-client.c \
	$(SRCDIR)/providers/ai-grok-home-overlay.c \
	$(SRCDIR)/convenience/ai-simple.c \
	$(SRCDIR)/convenience/ai-search-provider.c \
	$(SRCDIR)/convenience/ai-search-result.c \
	$(SRCDIR)/convenience/ai-search-options.c \
	$(SRCDIR)/convenience/ai-search-http.c \
	$(SRCDIR)/convenience/ai-bing-search.c \
	$(SRCDIR)/convenience/ai-brave-search.c \
	$(SRCDIR)/convenience/ai-duckduckgo-search.c \
	$(SRCDIR)/convenience/ai-tool-executor.c \
	$(SRCDIR)/convenience/ai-provider-factory.c \
	$(SRCDIR)/agent/ai-agent-enums.c \
	$(SRCDIR)/agent/ai-budget.c \
	$(SRCDIR)/agent/ai-price-table.c \
	$(SRCDIR)/agent/ai-agent-worker.c \
	$(SRCDIR)/agent/ai-agent-host.c \
	$(SRCDIR)/agent/ai-agent-store.c \
	$(SRCDIR)/agent/ai-agent-isolation.c \
	$(SRCDIR)/agent/ai-agent.c \
	$(SRCDIR)/agent/ai-brigade.c \
	$(SRCDIR)/agent/ai-local-worker.c \
	$(SRCDIR)/agent/ai-mock-provider.c \
	$(SRCDIR)/harness/ai-resource.c \
	$(SRCDIR)/harness/ai-resource-registry.c \
	$(SRCDIR)/harness/ai-mention.c \
	$(SRCDIR)/harness/ai-command.c \
	$(SRCDIR)/harness/ai-completion.c

# Object files
LIB_OBJECTS = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(LIB_SOURCES))

# Test files
TEST_SOURCES = $(wildcard $(TESTDIR)/test-*.c)
TEST_HEADERS = $(wildcard $(TESTDIR)/*.h)
TEST_BINARIES = $(patsubst $(TESTDIR)/%.c,$(OUTDIR)/tests/%,$(TEST_SOURCES))

# Example files
EXAMPLE_SOURCES = $(wildcard $(EXAMPLEDIR)/*.c)
EXAMPLE_BINARIES = $(patsubst $(EXAMPLEDIR)/%.c,$(OUTDIR)/examples/%,$(EXAMPLE_SOURCES))

# Installable CLI binaries (e.g. the `ai` front-end)
BIN_SOURCES = $(wildcard $(BINDIR)/*.c)

# ai-tui needs ncursesw; without it, drop it rather than fail the build.
ifneq ($(HAVE_NCURSES),1)
BIN_SOURCES := $(filter-out $(BINDIR)/ai-tui.c,$(BIN_SOURCES))
$(info Note: ncursesw not found, skipping ai-tui. Install ncurses-devel to build it.)
endif

BIN_BINARIES = $(patsubst $(BINDIR)/%.c,$(OUTDIR)/bin/%,$(BIN_SOURCES))

# Target-specific, so only the one binary that needs a terminal library
# links against one.
$(OUTDIR)/bin/ai-tui: CFLAGS += $(NCURSES_CFLAGS)
$(OUTDIR)/bin/ai-tui: LDFLAGS += $(NCURSES_LIBS)

# Include common rules
include rules.mk

# Build bundled yaml-glib static library.
# Recent yaml-glib refactored its build to split output by type
# (build/release/, build/debug/) and consolidated lib-static + lib-shared
# into a single `lib` target. We only consume the static archive but pay
# for the shared lib too — acceptable since yaml-glib is small.
$(YAML_GLIB_STATIC):
	@echo "Building yaml-glib..."
	$(MAKE) -C $(YAML_GLIB_DIR) lib DEBUG=0

# Default target
.PHONY: all
all: $(OUTDIR)/config.h $(OUTDIR)/ai-version.h shared static $(PROJECT_NAME)-1.0.pc gir binaries

# Generate config.h from template
$(OUTDIR)/config.h: $(SRCDIR)/config.h.in | $(OUTDIR)
	@echo "Generating config.h..."
	@sed -e 's/@VERSION_MAJOR@/$(VERSION_MAJOR)/g' \
	     -e 's/@VERSION_MINOR@/$(VERSION_MINOR)/g' \
	     -e 's/@VERSION_MICRO@/$(VERSION_MICRO)/g' \
	     -e 's/@PACKAGE_NAME@/$(PROJECT_NAME)/g' \
	     -e 's/@PACKAGE_VERSION@/$(VERSION)/g' \
	     -e 's|@PACKAGE_BUGREPORT@|$(PACKAGE_BUGREPORT)|g' \
	     -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
	     $< > $@

# Generate ai-version.h from template
$(OUTDIR)/ai-version.h: $(SRCDIR)/ai-version.h.in | $(OUTDIR)
	@echo "Generating ai-version.h..."
	@sed -e 's/@AI_GLIB_MAJOR_VERSION@/$(VERSION_MAJOR)/g' \
	     -e 's/@AI_GLIB_MINOR_VERSION@/$(VERSION_MINOR)/g' \
	     -e 's/@AI_GLIB_MICRO_VERSION@/$(VERSION_MICRO)/g' \
	     -e 's/@AI_GLIB_VERSION@/$(VERSION)/g' \
	     $< > $@

# Shared library
.PHONY: shared
shared: $(LIB_SHARED)

$(LIB_SHARED): $(LIB_OBJECTS) | $(OUTDIR)
	@echo "Linking shared library..."
	$(CC) -shared -Wl,-soname,$(LIB_SONAME) -o $@ $(LIB_OBJECTS) $(LDFLAGS)
	@cd $(OUTDIR) && ln -sf $(notdir $(LIB_SHARED)) $(LIB_SONAME) 2>/dev/null || true
	@cd $(OUTDIR) && ln -sf $(LIB_SONAME) $(LIB_NAME).so 2>/dev/null || true

# Static library
.PHONY: static
static: $(LIB_STATIC)

$(LIB_STATIC): $(LIB_OBJECTS) | $(OUTDIR)
	@echo "Creating static library..."
	$(AR) rcs $@ $(LIB_OBJECTS)

# Ensure config.h, ai-version.h, and yaml-glib exist before compiling
$(LIB_OBJECTS): $(OUTDIR)/config.h $(OUTDIR)/ai-version.h $(YAML_GLIB_STATIC)

# pkg-config file (stays at project root — moving it under $(OUTDIR) would
# silently break consumers that run `pkg-config --variable pcfiledir`).
$(PROJECT_NAME)-1.0.pc: $(PROJECT_NAME)-1.0.pc.in
	@echo "Generating pkg-config file..."
	@sed -e 's|@PREFIX@|$(PREFIX)|g' \
	     -e 's|@LIBDIR@|$(LIBDIR)|g' \
	     -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
	     -e 's|@VERSION@|$(VERSION)|g' \
	     $< > $@

# Tests
# tests/test-ai-cli.c spawns the installed-shaped `ai` binary, so the test
# run needs it built even when the caller only asked for tests.
.PHONY: test
test: $(TEST_BINARIES) $(BIN_BINARIES)
	@echo "Running tests..."
	@for test in $(TEST_BINARIES); do \
		echo "Running $$test..."; \
		$$test || exit 1; \
	done
	@$(MAKE) --no-print-directory test-gir-clean
	@echo "All tests passed!"

.PHONY: test-verbose
test-verbose: $(TEST_BINARIES) $(BIN_BINARIES)
	@echo "Running tests (verbose)..."
	@for test in $(TEST_BINARIES); do \
		echo "Running $$test..."; \
		G_TEST_VERBOSE=1 $$test || exit 1; \
	done
	@echo "All tests passed!"

# Run the Python GI binding smoke test. Skips cleanly when PyGObject is
# not installed on the host.
.PHONY: test-gi
test-gi:
	@if ! /usr/bin/python3 -c "import gi" >/dev/null 2>&1; then \
		echo "SKIP: python3-gobject not installed (Fedora: python3-gobject, Debian: python3-gi)"; \
		exit 0; \
	fi
	@if [ ! -f $(TYPELIB_FILE) ]; then \
		echo "Building typelib (GIR=1)..."; \
		$(MAKE) GIR=1 gir; \
	fi
	@echo "Running PyGObject smoke test..."
	LD_LIBRARY_PATH=$(OUTDIR) GI_TYPELIB_PATH=$(OUTDIR) \
		/usr/bin/python3 $(TESTDIR)/test-gi-bindings.py

# Assert the g-ir-scanner output is warning-free. CI gate that catches any
# regression that re-introduces annotation noise. Skips cleanly when
# gobject-introspection-devel is not installed on the host.
.PHONY: test-gir-clean
test-gir-clean:
	@if ! command -v $(GIR_SCANNER) >/dev/null 2>&1; then \
		echo "SKIP: $(GIR_SCANNER) not on PATH (install gobject-introspection-devel)"; \
	else \
		echo "Checking for GIR scanner warnings..."; \
		rm -f $(GIR_FILE) $(TYPELIB_FILE); \
		OUTPUT=$$($(MAKE) GIR=1 gir 2>&1); \
		WARN=$$(echo "$$OUTPUT" | grep -c "Warning:" || true); \
		if [ "$$WARN" != "0" ]; then \
			echo "FAIL: $$WARN GIR scanner warning(s):"; \
			echo "$$OUTPUT" | grep "Warning:" >&2; \
			exit 1; \
		fi; \
		echo "PASS: GIR scanner is clean (0 warnings)"; \
	fi

# Examples
.PHONY: examples
examples: $(EXAMPLE_BINARIES)

# Installable CLI binaries (the `ai` front-end). Built as part of `all`.
.PHONY: binaries
binaries: $(BIN_BINARIES)

# GObject introspection (opt-in: pass GIR=1).  Defaults off so hosts that
# lack gobject-introspection-devel can build without setting any flags.
ifeq ($(GIR),1)

.PHONY: gir
gir: $(TYPELIB_FILE)

$(GIR_FILE): $(LIB_SHARED) $(PUBLIC_HEADERS) | $(OUTDIR)
	@echo "Generating GObject introspection data..."
	$(GIR_SCANNER) --namespace=$(GIR_NAMESPACE) \
		--nsversion=$(GIR_VERSION) \
		--identifier-prefix=Ai \
		--symbol-prefix=ai \
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
		--library=$(PROJECT_NAME)-1.0 \
		--library-path=$(OUTDIR) \
		-I$(SRCDIR) \
		-I$(OUTDIR) \
		--output=$@ \
		$(PUBLIC_HEADERS) $(LIB_SOURCES)

$(TYPELIB_FILE): $(GIR_FILE)
	$(GIR_COMPILER) $< -o $@

# Install .gir and .typelib into standard GI search paths
.PHONY: install-gir
install-gir: gir
	install -d $(DESTDIR)$(LIBDIR)/girepository-1.0
	install -m 644 $(TYPELIB_FILE) \
		$(DESTDIR)$(LIBDIR)/girepository-1.0/$(GIR_NAMESPACE)-$(GIR_VERSION).typelib
	install -d $(DESTDIR)$(PREFIX)/share/gir-1.0
	install -m 644 $(GIR_FILE) \
		$(DESTDIR)$(PREFIX)/share/gir-1.0/$(GIR_NAMESPACE)-$(GIR_VERSION).gir

else

.PHONY: gir install-gir
gir install-gir:
	@echo "GIR disabled (GIR=0). Re-build with GIR=1 to produce .gir/.typelib."

endif

# Installation
.PHONY: install
install: all install-gir
	@echo "Installing to $(PREFIX)..."
	install -d $(DESTDIR)$(PREFIX)/bin
	@for b in $(BIN_BINARIES); do \
		echo "  install $$b"; \
		install -m 755 $$b $(DESTDIR)$(PREFIX)/bin/; \
	done
	install -d $(DESTDIR)$(LIBDIR)
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/core
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/model
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/providers
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/convenience
	install -d $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/agent
	install -d $(DESTDIR)$(PKGCONFIGDIR)
	install -m 644 $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(LIBDIR)/
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(notdir $(LIB_SHARED)) $(LIB_SONAME)
	cd $(DESTDIR)$(LIBDIR) && ln -sf $(LIB_SONAME) $(LIB_NAME).so
	install -m 644 $(SRCDIR)/ai-glib.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/
	install -m 644 $(SRCDIR)/ai-types.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/
	install -m 644 $(OUTDIR)/ai-version.h $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/
	install -m 644 $(filter $(SRCDIR)/core/%,$(PUBLIC_HEADERS)) $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/core/
	install -m 644 $(filter $(SRCDIR)/model/%,$(PUBLIC_HEADERS)) $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/model/
	install -m 644 $(filter $(SRCDIR)/providers/%,$(PUBLIC_HEADERS)) $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/providers/
	install -m 644 $(filter $(SRCDIR)/convenience/%,$(PUBLIC_HEADERS)) $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/convenience/
	install -m 644 $(filter $(SRCDIR)/agent/%,$(PUBLIC_HEADERS)) $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0/agent/
	install -m 644 $(PROJECT_NAME)-1.0.pc $(DESTDIR)$(PKGCONFIGDIR)/
	@echo "Installation complete!"

.PHONY: uninstall
uninstall:
	@echo "Uninstalling from $(PREFIX)..."
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME).so*
	rm -f $(DESTDIR)$(LIBDIR)/$(LIB_NAME).a
	rm -rf $(DESTDIR)$(INCLUDEDIR)/$(PROJECT_NAME)-1.0
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/$(PROJECT_NAME)-1.0.pc
	rm -f $(DESTDIR)$(LIBDIR)/girepository-1.0/$(GIR_NAMESPACE)-$(GIR_VERSION).typelib
	rm -f $(DESTDIR)$(PREFIX)/share/gir-1.0/$(GIR_NAMESPACE)-$(GIR_VERSION).gir
	@echo "Uninstallation complete!"

# Print variables (for debugging)
.PHONY: vars
vars:
	@echo "PROJECT_NAME  = $(PROJECT_NAME)"
	@echo "VERSION       = $(VERSION)"
	@echo "BUILD_TYPE    = $(BUILD_TYPE)"
	@echo "OUTDIR        = $(OUTDIR)"
	@echo "OBJDIR        = $(OBJDIR)"
	@echo "GIR           = $(GIR)"
	@echo "CC            = $(CC)"
	@echo "CFLAGS        = $(CFLAGS)"
	@echo "LDFLAGS       = $(LDFLAGS)"
	@echo "PREFIX        = $(PREFIX)"
	@echo "LIB_SOURCES   = $(LIB_SOURCES)"
	@echo "LIB_OBJECTS   = $(LIB_OBJECTS)"

endif # ifndef __MIXED
