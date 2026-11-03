#!/usr/bin/env bash
# Bu dosya manage.sh üzerindeki bölünmüş ekran monitör işlevini çağırır.
exec "$(dirname "$0")/manage.sh" monitor "$@"
