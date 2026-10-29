#!/bin/sh
# The manual against the program: every option `pond --help` names and every
# parameter `pond --list-params` lists must be in pond.1, and every option
# pond.1 names must be one the program takes.
#   sh tests/test_man.sh [./pond] [pond.1]
bin=${1:-./pond}
man=${2:-pond.1}
fail=0; bad=

# the page as plain text: roff's escaped hyphens and fonts removed
text=$(sed -e 's/\\-/-/g' -e 's/\\f[BIRP]//g' "$man")
has() {   # has WORD: WORD in the page, not as part of a longer name
    pat=$(printf '%s' "$1" | sed 's/\./\\./g')
    printf '%s\n' "$text" | grep -Eq -e "(^|[^A-Za-z0-9.-])$pat([^A-Za-z0-9-]|\$)"
}

opts=$("$bin" --help | grep -o -e '--[a-z0-9][a-z0-9-]*' | sort -u)
params=$("$bin" --list-params --no-config | awk '{ print $1 }')
[ -n "$opts" ] && [ -n "$params" ] || { echo "could not run $bin --help / --list-params"; exit 1; }

# --help's own options; its examples of parameters (--yaw, --no-hud) and the
# placeholders --name and --no-name are checked as parameters below
n=0
for o in $opts; do
    name=${o#--}; bare=${name#no-}
    case $name in name|no-name) continue ;; esac
    printf '%s\n' "$params" | grep -qx -e "$name" -e "$bare" && continue
    n=$((n + 1))
    has "$o" || { echo "pond.1 lacks the option $o"; bad=1; }
done
[ -z "$bad" ] && echo "options in --help: $n, all in pond.1"
fail=${bad:-$fail}; bad=

m=0
for p in $params; do
    m=$((m + 1))
    has "$p" || { echo "pond.1 lacks the parameter $p"; bad=1; }
done
[ -z "$bad" ] && echo "parameters: $m, all in pond.1"
fail=${bad:-$fail}; bad=

# the other way round: an option in the page is one in --help, or --name /
# --no-name for a parameter
k=0
for o in $(printf '%s\n' "$text" | grep -o -e '--[a-z0-9][a-z0-9.-]*[a-z0-9]' | sort -u); do
    k=$((k + 1))
    name=${o#--}; bare=${name#no-}
    printf '%s\n' "$opts" | grep -qx -e "$o" && continue
    printf '%s\n' "$params" | grep -qx -e "$name" && continue
    printf '%s\n' "$params" | grep -qx -e "$bare" && continue
    echo "pond.1 names $o, which pond does not take"; bad=1
done
[ -z "$bad" ] && echo "options in pond.1: $k, all real"
fail=${bad:-$fail}; bad=

# the page itself formats without complaint, where a formatter is to hand
if command -v mandoc >/dev/null 2>&1; then
    w=$(mandoc -T lint -W warning "$man" 2>&1)
    if [ -n "$w" ]; then echo "$w"; echo "mandoc: warnings"; fail=1; else echo "mandoc lint: clean"; fi
elif command -v groff >/dev/null 2>&1; then
    w=$(groff -man -ww -z "$man" 2>&1)
    if [ -n "$w" ]; then echo "$w"; echo "groff: warnings"; fail=1; else echo "groff: clean"; fi
fi

[ $fail = 0 ] && echo "all man tests passed"
exit $fail
