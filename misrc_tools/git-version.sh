#!/bin/sh
set -eu

V=$(git describe --tags --dirty --match 'misrc_tools-*' 2>/dev/null || true)

if [ -z "$V" ]; then
	SHA=$(git rev-parse --short HEAD 2>/dev/null || echo "nogit")
	DIRTY=""
	if git diff --quiet --ignore-submodules -- 2>/dev/null; then
		:
	else
		DIRTY="-dirty"
	fi
	V="misrc_gui-0.0.0-${SHA}${DIRTY}"
fi

printf '%s\n' "${V#misrc_tools-}"
