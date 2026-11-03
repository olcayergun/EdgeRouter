#!/usr/bin/env bash
# Bu dosya manage.sh üzerindeki upload işlevini çağırır.
exec "$(dirname "$0")/manage.sh" upload "$@"
