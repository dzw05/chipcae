#!/bin/sh
while [ "$#" -gt 0 ]; do
    if [ "${1##*/}" = "jobmgr_local_mock_solver" ]; then
        exec "$@"
    fi
    shift
done
exit 97
