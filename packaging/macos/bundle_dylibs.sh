#!/usr/bin/env bash
# Moved to packaging/common/bundle_dylibs.sh, shared with the title-app kit
# (packaging/title/). Kept so existing callers keep working.
exec "$(dirname "$0")/../common/bundle_dylibs.sh" "$@"
