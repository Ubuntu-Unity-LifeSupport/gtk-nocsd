#!/bin/sh
# Run the dialog/parent regression cases, each in its own process with a time
# limit, with the freshly built library preloaded. Called under xvfb-run.
# usage: run-dialog-parent.sh <library> <gtk4 test binary> <gtk3 test binary>

set -u

library=$1
gtk4_test=$2
gtk3_test=$3

runtime=$(mktemp -d)
trap 'rm -rf -- "${runtime:?}"' EXIT
chmod 700 "$runtime"

unset XDG_CURRENT_DESKTOP GTK_CSD GTK_THEME DESKTOP_SESSION
export GDK_BACKEND=x11 GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1 \
	GSETTINGS_BACKEND=memory ADW_DISABLE_PORTAL=1 \
	DBUS_SESSION_BUS_ADDRESS=disabled: \
	HOME="$runtime" XDG_RUNTIME_DIR="$runtime" \
	XDG_CONFIG_HOME="$runtime/config" XDG_CACHE_HOME="$runtime/cache" \
	NOCSD_TEST_EXPECT_PRELOAD=1

failed=""
run() {
	name=$1
	shift
	echo "=== $name"
	LD_PRELOAD="$library" timeout -k 5 90 "$@"
	rc=$?
	if [ "$rc" -eq 0 ]; then
		echo "=== $name: PASS"
	else
		echo "=== $name: FAIL (exit $rc)"
		failed="$failed $name"
	fi
}

for case in T1-app-destroy T1b-app-close T2-plain-window T3-nested-dialog \
	T4-represent-kept-dialog T5-can-close-false control-closed-first \
	control-represent-after-close control-hide-on-close-parent \
	T6-two-dialogs; do
	run "$case" "$gtk4_test" "$case"
done
run control-gtk3 "$gtk3_test"

if [ -n "$failed" ]; then
	echo "dialog/parent tests failed:$failed"
	exit 1
fi
echo "dialog/parent tests: all passed"
