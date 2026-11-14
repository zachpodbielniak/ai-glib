#!/bin/bash
# ai-glib - GLib/GObject library for AI providers
# Copyright (C) 2026  Zach Podbielniak
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# fake-codex-app-server.bash - a scripted stand-in for `codex app-server`
#
# tests/test-codex-app-server.c copies this file into a temporary
# directory as `codex` and points AiCodexCliClient at it.  It speaks
# nothing by itself: it plays the script the test wrote, so every
# request the client sends is recorded and every answer -- results,
# JSON-RPC errors, silence -- is the test's choice.
#
# FAKE_CODEX_DIR names the directory.  It holds:
#
#   script        one action per line, played in order:
#                   read         read one line from stdin, append it to
#                                requests.log (fails if stdin is closed)
#                   send JSON    write JSON to stdout as one line
#                   sleep SECS   pause
#                   eof          wait for EOF on stdin, then write `eof`
#                   exit CODE    stop at once with CODE
#   argv.log      written: the arguments it was started with
#   requests.log  written: every line read
#   eof           written by `eof`: proof the client closed stdin
#
# The script ends with exit 0.  A client that never closes stdin leaves
# `eof` waiting until the client's own process deadline kills it.

set -euo pipefail

fake_codex_main () {
    local dir="${FAKE_CODEX_DIR:?FAKE_CODEX_DIR must name the fixture directory}"
    local action=""
    local line=""

    printf '%s\n' "$*" > "${dir}/argv.log"
    exec 3< "${dir}/script"

    while IFS= read -r action <&3
    do
        case "${action}" in
            read)
                if ! IFS= read -r line
                then
                    echo "fake codex: stdin closed while a request was expected" >&2
                    exit 3
                fi
                printf '%s\n' "${line}" >> "${dir}/requests.log"
                ;;
            send\ *)
                printf '%s\n' "${action#send }"
                ;;
            sleep\ *)
                sleep "${action#sleep }"
                ;;
            eof)
                cat > /dev/null
                echo eof > "${dir}/eof"
                ;;
            exit\ *)
                exit "${action#exit }"
                ;;
            '')
                ;;
            *)
                echo "fake codex: unknown action: ${action}" >&2
                exit 2
                ;;
        esac
    done

    exit 0
}

fake_codex_main "$@"
