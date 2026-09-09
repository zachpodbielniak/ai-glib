# Gates: ai-tui resilience and composer editing

OWNS: bin/ai-tui.c, tests/test-ai-tui.c, docs/tui.org, GATES.md

Scope: Resolve command paths against the conversation directory and add Unicode-safe forward delete, word kill, line kill and yank with terminal regression coverage and Org documentation in a committed worktree.

- [x] G1: All targets and tests compile with warnings treated as errors
  CHECK: make clean && make -j4 all tests WARNINGS='-Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wshadow -Werror'
  EXPECT: check-headers: OK
  EVIDENCE: exit=0; shell=/bin/sh; cwd=/var/home/zach/source/projects/ai-glib/trees/tui-resilience; path=2bc5e3beb754/14 entries; EXPECT=matched; output-sha256=dbe9388d175011f0aa7f0fde1b31c199365dd2c892e567a1bb34abcbdea33e17; output-bytes=127025

- [x] G2: The regression suite including terminal editing and path commands passes
  CHECK: make test WARNINGS='-Wall -Wextra -Wno-unused-parameter -Wformat=2 -Wshadow -Werror' > build/tui-regression.log 2>&1 && tail -n 1 build/tui-regression.log
  EXPECT: All tests passed!
  EVIDENCE: exit=0; shell=/bin/sh; cwd=/var/home/zach/source/projects/ai-glib/trees/tui-resilience; path=2bc5e3beb754/14 entries; EXPECT=matched; output-sha256=9216276c831b1af0d76031b4c34b2a4db354d52ebbc0b4b5142577f8a0cb477d; output-bytes=18

- [x] G3: Documentation describes each new binding and path resolution; conventional commits contain the tested changes in the requested worktree
  EVIDENCE: Reviewed docs/tui.org Composer editing and Command paths against the implementation and terminal regression tests. Commit b43603d (feat(tui): add kill and yank editing and resolve session paths consistently) contains code, tests and Org documentation on feat/tui-resilience in trees/tui-resilience. Eight editing scenarios verify provider input; command-path coverage checks nested relative changes, absolute and home paths, spaces and recovery after failures. The full suite reports 1990 TAP results including three explicitly skipped live network tests; its final GIR scanner check reports zero warnings. git diff --check passed before committing.
