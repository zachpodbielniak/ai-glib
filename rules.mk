# rules.mk - Common build rules for ai-glib
#
# Copyright (C) 2025
# SPDX-License-Identifier: AGPL-3.0-or-later

# Pattern rules for object files
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/core/%.o: $(SRCDIR)/core/%.c | $(OBJDIR)/core
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/model/%.o: $(SRCDIR)/model/%.c | $(OBJDIR)/model
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/providers/%.o: $(SRCDIR)/providers/%.c | $(OBJDIR)/providers
	$(CC) $(CFLAGS) -c $< -o $@

# Create build directories
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/core:
	mkdir -p $(OBJDIR)/core

$(OBJDIR)/model:
	mkdir -p $(OBJDIR)/model

$(OBJDIR)/providers:
	mkdir -p $(OBJDIR)/providers

# Test compilation rule
$(BUILDDIR)/tests/%: $(TESTDIR)/%.c $(LIB_SHARED)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRCDIR) $< -o $@ -L$(BUILDDIR) -l$(PROJECT_NAME)-1.0 $(LDFLAGS) -Wl,-rpath,$(BUILDDIR)

# Example compilation rule
$(BUILDDIR)/examples/%: $(EXAMPLEDIR)/%.c $(LIB_SHARED)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRCDIR) $< -o $@ -L$(BUILDDIR) -l$(PROJECT_NAME)-1.0 $(LDFLAGS) -Wl,-rpath,$(BUILDDIR)

# Clean rule
.PHONY: clean
clean:
	rm -rf $(BUILDDIR)
	rm -f $(PROJECT_NAME)-1.0.pc
	$(MAKE) -C $(YAML_GLIB_DIR) clean 2>/dev/null || true

# Distclean rule
.PHONY: distclean
distclean: clean
	rm -f build/config.h
	rm -f build/ai-version.h

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
	@echo "  gir          - Generate GObject introspection data"
	@echo "  install      - Install library and headers"
	@echo "  uninstall    - Uninstall library and headers"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove all generated files"
	@echo "  help         - Show this help"
	@echo ""
	@echo "Build options:"
	@echo "  DEBUG=1      - Build with debug symbols and no optimization"
	@echo "  ASAN=1       - Enable address sanitizer"
	@echo "  UBSAN=1      - Enable undefined behavior sanitizer"
	@echo "  PREFIX=/path - Set installation prefix (default: /usr/local)"
