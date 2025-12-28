#!/bin/sh
set -eu

# Prefer the annotated/tagged version format.
if V=$(git describe --tags --dirty --match 'misrc_tools-*' 2>/dev/null); then
	echo "$V" | cut -c 13-
	exit 0
fi

# Fallback for repos without tags (or tarball builds).
if H=$(git rev-parse --short HEAD 2>/dev/null); then
	echo "dev-$H"
	exit 0
fi

echo "dev"
