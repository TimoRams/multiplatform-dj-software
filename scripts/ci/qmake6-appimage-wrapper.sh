#!/usr/bin/env bash
set -euo pipefail

: "${QMAKE_REAL:?QMAKE_REAL must point to the real Qt 6 qmake}"
: "${QT_PLUGIN_STAGE:?QT_PLUGIN_STAGE must point to the isolated plugin tree}"

if [[ $# -eq 1 && "$1" == "-query" ]]; then
    "$QMAKE_REAL" -query | sed "s|^QT_INSTALL_PLUGINS:.*|QT_INSTALL_PLUGINS:${QT_PLUGIN_STAGE}|"
elif [[ $# -eq 2 && "$1" == "-query" && "$2" == "QT_INSTALL_PLUGINS" ]]; then
    echo "$QT_PLUGIN_STAGE"
else
    exec "$QMAKE_REAL" "$@"
fi
