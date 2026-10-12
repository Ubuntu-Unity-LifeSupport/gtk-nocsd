#!/bin/sh
# Test for debian/51gtk-nocsd. Usage: test-51gtk-nocsd.sh SCRIPT SHELL [plain|strict]
# Fake systemctl prints $FAKE_ENV; /etc and $HOME are redirected into a
# temporary directory.
set -u
S=$1; SH=$2; STRICT=${3:-}
W=$(mktemp -d)
mkdir -p "$W/bin" "$W/etc/environment.d" "$W/run/environment.d" "$W/usrlocal/environment.d" "$W/home/.config/environment.d"
cat > "$W/bin/systemctl" <<'EOF'
#!/bin/sh
[ -n "${FAKE_ENV+x}" ] && printf '%s\n' "HOME=/home/x" "$FAKE_ENV"
exit 0
EOF
chmod +x "$W/bin/systemctl"
: > "$W/50-gtk-nocsd.conf"
sed -e "s|/usr/lib/environment.d/50-gtk-nocsd.conf|$W/50-gtk-nocsd.conf|" \
    -e "s| /etc /run /usr/local/lib | $W/etc $W/run $W/usrlocal |" "$S" > "$W/51"
grep -q "$W/etc $W/run $W/usrlocal" "$W/51" || { echo "FAIL harness: opt-out dirs not redirected"; exit 1; }
fail=0
run() { # name, initial LD_PRELOAD ("-" unset), FAKE_ENV ("-" none), expected ("-" untouched), [optout]
  name=$1; init=$2; fake=$3; want=$4; opt=${5:-}
  rm -f "$W/etc/environment.d/50-gtk-nocsd.conf" "$W/run/environment.d/50-gtk-nocsd.conf" "$W/usrlocal/environment.d/50-gtk-nocsd.conf" "$W/home/.config/environment.d/50-gtk-nocsd.conf"
  case $opt in
    etc) : > "$W/etc/environment.d/50-gtk-nocsd.conf" ;;
    run) : > "$W/run/environment.d/50-gtk-nocsd.conf" ;;
    usrlocal) : > "$W/usrlocal/environment.d/50-gtk-nocsd.conf" ;;
    home) : > "$W/home/.config/environment.d/50-gtk-nocsd.conf" ;;
  esac
  got=$(PATH="$W/bin:$PATH" HOME="$W/home" W="$W" INIT="$init" FAKE="$fake" STRICT="$STRICT" "$SH" -c '
    if [ "$STRICT" = strict ]; then set -eu; fi
    unset XDG_CONFIG_HOME
    if [ "$FAKE" = "-" ]; then unset FAKE_ENV; else FAKE_ENV=$FAKE; export FAKE_ENV; fi
    if [ "$INIT" = "-" ]; then unset LD_PRELOAD; else LD_PRELOAD=$INIT; fi
    IFS_BEFORE=$IFS
    STARTUP=/usr/bin/x-session-manager
    . "$W/51"
    [ "$IFS" = "$IFS_BEFORE" ] || echo IFS-CHANGED
    for v in _nocsd_v _nocsd_new _nocsd_found _nocsd_rest _nocsd_e; do
      eval "[ -z \"\${$v+x}\" ]" || echo "LEFT-$v"
    done
    printf "%s|%s" "${LD_PRELOAD-UNSET}" "$STARTUP"
  ' 2>&1)
  if [ "$want" = "-" ]; then
    exp="${init_unset:-}"
    case $init in -) exp="UNSET|/usr/bin/x-session-manager" ;; *) exp="$init|/usr/bin/x-session-manager" ;; esac
  else
    exp="$want|env LD_PRELOAD=$want /usr/bin/x-session-manager"
  fi
  if [ "$got" = "$exp" ]; then echo "PASS $name"; else echo "FAIL $name: got [$got] want [$exp]"; fail=1; fi
}
M='/usr/$LIB/libunity-gtk4-menu.so.0'
N='/usr/$LIB/libgtk-nocsd.so.0'
run "manager path form decoded"     -  "LD_PRELOAD=\$'$M:$N'"        "$M:$N"
run "manager old bare names"        -  "LD_PRELOAD=libunity-gtk4-menu.so.0:libgtk-nocsd.so.0" "$M:$N"
run "manager only gtk4-menu"        -  "LD_PRELOAD=\$'$M'"            "$N:$M"
run "manager only bare gtk4-menu"   -  "LD_PRELOAD=libunity-gtk4-menu.so.0" "$N:$M"
run "manager without LD_PRELOAD"    -  "PATH=/usr/bin"                 "$N"
run "no user manager"               -  "-"                             "$N"
run "manager value other escape"    -  "LD_PRELOAD=\$'/a\\n/b'"        "$N"
run "manager value escaped quote"   -  "LD_PRELOAD=\$'/x\\'y/\$LIB/l.so'" "$N:/x'y/\$LIB/l.so"
run "manager value escaped bslash"  -  "LD_PRELOAD=\$'/x\\\\y/\$LIB/l.so'" "$N:/x\\y/\$LIB/l.so"
run "manager unclosed quote"        -  "LD_PRELOAD=\$'/usr/\$LIB/a.so"  "$N"
run "manager value with space"      -  "LD_PRELOAD=\$'/opt/a.so $N'"   "/opt/a.so:$N"
run "manager mixed + user bare dup" -  "LD_PRELOAD=\$'$M:$N:libgtk-nocsd.so.0'" "$M:$N:$N"
run "set by user, missing"          "/opt/mylib.so" "-"               "$N:/opt/mylib.so"
run "set by user, expanded path"    "/usr/lib/x86_64-linux-gnu/libgtk-nocsd.so.0" "-" "/usr/lib/x86_64-linux-gnu/libgtk-nocsd.so.0"
run "set by user, spaces and tabs"  "/opt/a.so  libgtk-nocsd.so.0	/opt/b.so" "-" "/opt/a.so:$N:/opt/b.so"
run "set by user, empty entries"    "::/opt/a.so::" "-"               "$N:/opt/a.so"
run "similar name not counted"      "/opt/libgtk-nocsd.so.0.bak" "-"  "$N:/opt/libgtk-nocsd.so.0.bak"
run "opt-out /etc, manager empty"   -  "PATH=/usr/bin"                 "-" etc
run "opt-out home, manager empty"   -  "PATH=/usr/bin"                 "-" home
run "opt-out home, manager gtk4"    -  "LD_PRELOAD=\$'$M'"            "$M" home
run "opt-out, old bare nocsd kept as path" - "LD_PRELOAD=libgtk-nocsd.so.0" "$N" etc
run "opt-out /run, manager empty"   -  "PATH=/usr/bin"                 "-" run
run "opt-out /usr/local/lib"        -  "PATH=/usr/bin"                 "-" usrlocal
NL='
'
run "set by user, newline"          "/opt/a.so${NL}/opt/b.so" "-"     "$N:/opt/a.so:/opt/b.so"
rm -rf "${W:?}"
exit $fail
