# Gates: Native herdr integration

OWNS: Makefile bin/ai-tui.c bin/ai-tui-herdr.h tests/test-ai-tui-herdr.c tests/test-ai-tui.c tests/test-ai-defaults.c docs/herdr.org docs/tui.org README.org GATES.md

Scope: Automatically integrate ai-tui with herdr's local socket in a feature worktree, with bounded transport, lifecycle coverage, complete org documentation and a conventional commit.

- [x] G1: Release library and binaries build without compiler warnings.
  CHECK: env -u HERDR_ENV LC_ALL=C make -j8 clean all > /tmp/ai-glib-herdr-all.log 2>&1 && ! rg 'warning:|error:' /tmp/ai-glib-herdr-all.log && printf 'Warning-free release build passed\n'
  EXPECT: Warning-free release build passed
  EVIDENCE: exit=0; shell=/bin/sh; cwd=/var/home/zach/source/projects/ai-glib/trees/feat-herdr-integration; path=e2654908a384/14 entries; EXPECT=matched; output-sha256=9497d32202765220147999842dfb5acab9a0a08d5a5f510eab186233b1bf4182; output-bytes=34

- [x] G2: All test targets compile without warnings and the full suite passes.
  CHECK: env -u HERDR_ENV LC_ALL=C make -j8 clean tests > /tmp/ai-glib-herdr-tests-build.log 2>&1 && ! rg 'warning:|error:' /tmp/ai-glib-herdr-tests-build.log && env -u HERDR_ENV make test > /tmp/ai-glib-herdr-suite.log 2>&1 && ! rg 'warning:|error:' /tmp/ai-glib-herdr-suite.log && printf 'Warning-free full suite passed\n'
  EXPECT: Warning-free full suite passed
  EVIDENCE: exit=0; shell=/bin/sh; cwd=/var/home/zach/source/projects/ai-glib/trees/feat-herdr-integration; path=e2654908a384/14 entries; EXPECT=matched; output-sha256=df31416cb90fada0e60623a0f85c6e9fbfc5d8e252fef3a56b3f4ad0f8163dbd; output-bytes=31

- [x] G3: Socket tests verify detection, framing, failures, recovery, lifecycle ordering and bounded shutdown.
  CHECK: build/release/tests/test-ai-tui-herdr
  EXPECT: End of herdr tests
  EVIDENCE: exit=0; shell=/bin/sh; cwd=/var/home/zach/source/projects/ai-glib/trees/feat-herdr-integration; path=e2654908a384/14 entries; EXPECT=matched; output-sha256=a9e48b448ff067945d8d9c2fe61171960388484408f702190c367084a0d4ca74; output-bytes=2051

- [x] G4: Real herdr 0.8.2 accepts the integration and reflects ai-tui lifecycle in an isolated session.
  EVIDENCE: Installed herdr --version returned 0.8.2. Isolated headless server at /tmp/ai-glib-herdr-live/herdr.sock, with disposable configuration and stub grok, accepted the built ai-tui without hooks. pane.get w1:p1 returned agent=ai-tui/state=idle at startup, working during the stub's two-second turn, idle after completion, and no agent/state=unknown after /quit. No user server or provider service was contacted.

- [x] G5: Org documentation covers setup, protocol, state mapping, troubleshooting, limits and primary research sources; changes are committed conventionally in the worktree.
  EVIDENCE: Reviewed docs/herdr.org against implementation and herdr v0.8.2 source; linked from README.org and docs/tui.org, including --no-herdr in the option table. Feature commit 4ba7191 (feat(tui): integrate natively with herdr pane lifecycle) contains implementation, 35 passing integration tests, existing fixture isolation and complete org documentation in trees/feat-herdr-integration. Final full suite reports 1972 TAP results, including 3 explicitly skipped external-network tests; compiler and GIR warning counts are zero. The warning-search oracle matched a positive warning control. A relocated test executable verified the no-ai-tui optional-build path explicitly skips application tests. git diff --cached --check passed before commit.
