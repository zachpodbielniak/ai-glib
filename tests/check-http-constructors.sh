#!/bin/bash
#
# check-http-constructors.sh - Every SoupMessage and SoupSession goes through ai-http-redirect
#
# Copyright (C) 2026
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This file is part of ai-glib.
#
# ai_http_message_new() makes the caller say what a request carries --
# AI_HTTP_CARRIES_CREDENTIALS, AI_HTTP_NO_CREDENTIALS or
# AI_HTTP_PUBLIC_URL -- and guards it before anyone can add a header.
# ai_http_session_new() guards a session, so a message that reaches it by
# any other road is still judged by its headers when it is queued.  A
# bare soup_message_new*() or soup_session_new*() anywhere else is a
# request whose redirect behaviour nobody decided, and libsoup's default
# is to follow: Authorization, x-api-key and (on a 307/308) the prompt
# included.  That is how v0.11.0's fix stayed incomplete -- it guarded
# some sessions, and twenty-odd constructors kept building messages the
# guard never saw.
#
# A unit test cannot see this: test-http-redirect exercises the helper
# and passes whatever the call sites do.  So the rule is checked, not
# remembered.
#
# Scanned: src/, bin/ and gui/ (*.c, *.h), except src/core/ai-http-redirect.c,
# the one place the bare constructors are called.  Not scanned: tests/,
# which talk to their own loopback servers, and deps/, which carry their
# own guards (mcp-glib has the same check).
#
# Comments and string and character literals are blanked before matching,
# so a paragraph that explains the rule -- as ai-http-redirect-private.h's
# does -- is not reported as a violation of it, and an Accept header's
# "*/*" cannot open or close a comment.
#
# Usage:
#   tests/check-http-constructors.sh              check this tree
#   tests/check-http-constructors.sh --root DIR   check another tree
#   tests/check-http-constructors.sh --self-test  prove it fails on probes
#   tests/check-http-constructors.sh --help

set -euo pipefail

PATTERN='\bsoup_(message_new(_from_uri|_from_encoded_form|_from_multipart|_options_ping)?|session_new(_with_options)?)[[:space:]]*\('

usage () {
    cat <<'EOF'
check-http-constructors.sh - fail on a bare libsoup message or session constructor

Usage:
  check-http-constructors.sh [--root DIR]
  check-http-constructors.sh --self-test
  check-http-constructors.sh --help | --license

Options:
  --root DIR    the ai-glib tree to check (default: this script's tree)
  --self-test   run the check on probe trees and fail unless it catches them
  -h, --help    show this help
  --license     show the license

Examples:
  tests/check-http-constructors.sh
  tests/check-http-constructors.sh --root /tmp/ai-glib-copy
  tests/check-http-constructors.sh --self-test
EOF
}

# Blanks /* */ and // comments and the contents of "..." and '...'
# literals, one character at a time, keeping every newline so the line
# numbers it reports are the file's.
strip_comments_and_strings () {
    if [[ $# -ne 1 ]]
    then
        echo 'strip_comments_and_strings() requires 1 positional argument' >&2
        exit 2
    fi

    local file="${1}"

    awk '
    BEGIN { state = 0 }   # 0 code, 1 block comment, 2 string, 3 char
    {
        line = $0
        out = ""
        n = length(line)
        i = 1

        while (i <= n) {
            c = substr(line, i, 1)
            d = substr(line, i, 2)

            if (state == 1) {
                if (d == "*/") { state = 0; out = out "  "; i += 2 }
                else           { out = out " "; i++ }
            } else if (state == 2 || state == 3) {
                q = (state == 2) ? "\"" : "\047"
                if (c == "\\")   { out = out "  "; i += 2 }
                else if (c == q) { state = 0; out = out c; i++ }
                else             { out = out " "; i++ }
            } else if (d == "/*") {
                state = 1; out = out "  "; i += 2
            } else if (d == "//") {
                break
            } else if (c == "\"") {
                state = 2; out = out c; i++
            } else if (c == "\047") {
                state = 3; out = out c; i++
            } else {
                out = out c; i++
            }
        }

        # A literal does not span lines unless escaped; a comment does.
        if (state == 2 || state == 3)
            state = 0

        print out
    }' "${file}"
}

# Prints every violation under the tree at $1 and returns 1 if there is
# any, 0 if there is none.
check_tree () {
    if [[ $# -ne 1 ]]
    then
        echo 'check_tree() requires 1 positional argument' >&2
        exit 2
    fi

    local root="${1}"
    local bad=0
    local file
    local hits
    local -a dirs=()
    local dir

    for dir in src bin gui
    do
        [[ -d "${root}/${dir}" ]] && dirs+=("${root}/${dir}")
    done

    if [[ ${#dirs[@]} -eq 0 ]]
    then
        echo "http-constructors: no src/, bin/ or gui/ under ${root}" >&2
        return 1
    fi

    while IFS= read -r file
    do
        # Line numbers come from the blanked copy; the lines printed are
        # the file's own, so the report shows the literal arguments.
        hits=$(strip_comments_and_strings "${file}" | grep -nE "${PATTERN}" | cut -d: -f1 || true)

        if [[ -n "${hits}" ]]
        then
            printf 'http-constructors: %s\n' "${file#"${root}"/}" >&2
            awk -v lines="${hits//$'\n'/ }" '
                BEGIN { split(lines, l, " "); for (k in l) want[l[k]] = 1 }
                (FNR in want) { printf "http-constructors:   %d:%s\n", FNR, $0 }
            ' "${file}" >&2
            bad=1
        fi
    done < <(find "${dirs[@]}" -name '*.[ch]' \
                  ! -path '*/src/core/ai-http-redirect.c' | sort)

    if [[ "${bad}" -ne 0 ]]
    then
        cat >&2 <<'EOF'
http-constructors: FAILED
http-constructors: build the message with ai_http_message_new() or
http-constructors: ai_http_message_new_from_uri() from
http-constructors: core/ai-http-redirect-private.h and say what it carries
http-constructors: (AI_HTTP_CARRIES_CREDENTIALS never follows a redirect), and
http-constructors: the session with ai_http_session_new(); turn a refused
http-constructors: redirect into an error with ai_http_redirect_refused().
EOF
        return 1
    fi

    # The helper must still be what applies the rule; a refactor that
    # drops it would leave every call site above looking compliant.
    if ! grep -q 'SOUP_MESSAGE_NO_REDIRECT' "${root}/src/core/ai-http-redirect.c" ||
       ! grep -q 'ai_http_guard_session(session)' "${root}/src/core/ai-http-redirect.c"
    then
        echo 'http-constructors: src/core/ai-http-redirect.c no longer sets SOUP_MESSAGE_NO_REDIRECT or guards its sessions' >&2
        return 1
    fi

    return 0
}

# Builds probe trees in a temporary directory and checks that the guard
# fails on each bare constructor and passes on the decoys around it.
self_test () {
    local tmp
    local probe
    local failures=0

    tmp=$(mktemp -d)
    # shellcheck disable=SC2064  # expand now: tmp is local
    trap "rm -rf -- '${tmp}'" RETURN

    # A helper that still does its job, so only the probe decides.
    mkdir -p "${tmp}/clean/src/core"
    printf '%s\n' \
        'session = soup_session_new(); ai_http_guard_session(session);' \
        'soup_message_add_flags(m, SOUP_MESSAGE_NO_REDIRECT);' \
        > "${tmp}/clean/src/core/ai-http-redirect.c"

    # Decoys that must pass: the name in a comment, in a string next to
    # an Accept header's "*/*", a // comment, a longer identifier, and the
    # helper's own calls.
    cat > "${tmp}/clean/src/decoys.c" <<'EOF'
/* soup_message_new ("GET", url) is banned; see the helper. */
static const char *accept = "text/html,*/*;q=0.8";
static const char *name = "soup_session_new()";
// soup_message_new ("GET", url)
msg = ai_http_message_new ("GET", url, AI_HTTP_NO_CREDENTIALS);
x = my_soup_message_new_wrapper (y);
EOF

    if ! check_tree "${tmp}/clean" 2>/dev/null
    then
        echo 'http-constructors self-test: decoys were reported as violations' >&2
        failures=1
    fi

    # Each probe is one bare constructor, some hidden behind something
    # a naive comment stripper gets wrong.
    local -a probes=(
        'msg = soup_message_new ("GET", url);'
        'msg = soup_message_new_from_uri ("POST", uri);'
        'msg = soup_message_new_from_encoded_form ("POST", url, form);'
        'msg = soup_message_new_from_multipart (url, multipart);'
        'msg = soup_message_new_options_ping (uri);'
        's = soup_session_new ();'
        's = soup_session_new_with_options ("timeout", 30, NULL);'
        'h = "*/*"; msg = soup_message_new("GET", u); /* */'
        '/* a comment */ msg = soup_message_new("GET", u);'
    )
    local i
    local dir

    for i in "${!probes[@]}"
    do
        probe="${tmp}/probe-${i}"
        cp -r "${tmp}/clean" "${probe}"
        printf '%s\n' "${probes[${i}]}" > "${probe}/src/probe.c"

        if check_tree "${probe}" 2>/dev/null
        then
            printf 'http-constructors self-test: missed probe: %s\n' "${probes[${i}]}" >&2
            failures=1
        fi
    done

    # The same probe under bin/ and gui/ is caught too.
    for dir in bin gui
    do
        probe="${tmp}/probe-${dir}"
        cp -r "${tmp}/clean" "${probe}"
        mkdir -p "${probe}/${dir}"
        printf '%s\n' 'msg = soup_message_new ("GET", url);' > "${probe}/${dir}/probe.c"

        if check_tree "${probe}" 2>/dev/null
        then
            printf 'http-constructors self-test: missed a probe in %s/\n' "${dir}" >&2
            failures=1
        fi
    done

    # A helper that stopped refusing is caught even with clean call sites.
    probe="${tmp}/probe-helper"
    cp -r "${tmp}/clean" "${probe}"
    printf '%s\n' 'session = soup_session_new();' > "${probe}/src/core/ai-http-redirect.c"

    if check_tree "${probe}" 2>/dev/null
    then
        echo 'http-constructors self-test: a helper without the guard passed' >&2
        failures=1
    fi

    if [[ "${failures}" -ne 0 ]]
    then
        echo 'http-constructors self-test: FAILED' >&2
        return 1
    fi

    echo "http-constructors self-test: OK (${#probes[@]} probes and 3 tree probes caught, decoys passed)"
}

main () {
    local root

    root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)

    case "${1:-}" in
        -h|--help)
            usage
            return 0
            ;;
        --license)
            echo 'check-http-constructors.sh is part of ai-glib.'
            echo 'License: AGPL-3.0-or-later <https://www.gnu.org/licenses/agpl-3.0.html>'
            return 0
            ;;
        --self-test)
            self_test
            return
            ;;
        --root)
            if [[ $# -ne 2 ]]
            then
                usage >&2
                return 2
            fi
            root="${2}"
            ;;
        '')
            ;;
        *)
            usage >&2
            return 2
            ;;
    esac

    check_tree "${root}"
    echo 'http-constructors: OK (every SoupMessage and SoupSession decided where it may be redirected)'
}

main "$@"
