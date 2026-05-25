# rules.mk - Common build rules for ai-glib
#
# Copyright (C) 2025
# SPDX-License-Identifier: AGPL-3.0-or-later

# Object subdirectories (derived from source list)
OBJ_DIRS := $(sort $(dir $(LIB_OBJECTS)))

# Generic pattern rule for object files.
# Order-only deps on $(OUTDIR)/config.h and $(OUTDIR)/ai-version.h prevent a
# parallel-build race; the per-objects regular prereq in Makefile triggers
# recompilation when either header changes.
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJ_DIRS) $(OUTDIR)/config.h $(OUTDIR)/ai-version.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Create build directories
$(OUTDIR):
	mkdir -p $(OUTDIR)

$(OBJDIR): | $(OUTDIR)
	mkdir -p $(OBJDIR)

$(OBJ_DIRS): | $(OBJDIR)
	mkdir -p $@

# Test compilation rule
$(OUTDIR)/tests/%: $(TESTDIR)/%.c $(LIB_SHARED) | $(OUTDIR)/tests
	$(CC) $(CFLAGS) -I$(SRCDIR) $< -o $@ -L$(OUTDIR) -l$(PROJECT_NAME)-1.0 $(LDFLAGS) -Wl,-rpath,$(OUTDIR)

$(OUTDIR)/tests: | $(OUTDIR)
	mkdir -p $@

# Example compilation rule
$(OUTDIR)/examples/%: $(EXAMPLEDIR)/%.c $(LIB_SHARED) | $(OUTDIR)/examples
	$(CC) $(CFLAGS) -I$(SRCDIR) $< -o $@ -L$(OUTDIR) -l$(PROJECT_NAME)-1.0 $(LDFLAGS) -Wl,-rpath,$(OUTDIR)

$(OUTDIR)/examples: | $(OUTDIR)
	mkdir -p $@

# Clean current build type and the bundled yaml-glib build
.PHONY: clean
clean:
	rm -rf $(OUTDIR)
	rm -f $(PROJECT_NAME)-1.0.pc
	$(MAKE) -C $(YAML_GLIB_DIR) clean 2>/dev/null || true

# Clean everything (both build types + deps)
.PHONY: clean-all
clean-all:
	rm -rf $(BUILDDIR)
	rm -f $(PROJECT_NAME)-1.0.pc
	$(MAKE) -C $(YAML_GLIB_DIR) clean 2>/dev/null || true

# Distclean is an alias for clean-all
.PHONY: distclean
distclean: clean-all

# Help
.PHONY: help
help:
	@echo "ai-glib build system"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build shared and static libraries (default)"
	@echo "  shared       - Build shared library only"
	@echo "  static       - Build static library only"
	@echo "  test         - Build and run tests"
	@echo "  test-verbose - Build and run tests with verbose output"
	@echo "  examples     - Build example programs"
	@echo "  gir          - Generate GObject introspection data (requires GIR=1)"
	@echo "  install      - Install library and headers"
	@echo "  uninstall    - Uninstall library and headers"
	@echo "  clean        - Remove current build type ($(BUILD_TYPE)) and dep builds"
	@echo "  clean-all    - Remove all build artifacts and dep builds"
	@echo "  distclean    - Same as clean-all"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Build options:"
	@echo "  DEBUG=1      - Build with debug symbols and no optimization"
	@echo "  GIR=1        - Build GObject introspection (.gir/.typelib)"
	@echo "  ASAN=1       - Enable address sanitizer"
	@echo "  UBSAN=1      - Enable undefined behavior sanitizer"
	@echo "  PREFIX=/path - Set installation prefix (default: /usr/local)"
	@echo ""
	@echo "Output directories:"
	@echo "  make         -> build/release/"
	@echo "  make DEBUG=1 -> build/debug/"
