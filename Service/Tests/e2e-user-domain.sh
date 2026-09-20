#!/bin/sh
# End-to-end check of Sona Audio Service without sudo: bootstraps the freshly built service into
# the caller's launchd domain with a test peer requirement, drives it with the loopback clients,
# and boots it out again so it does not keep shadowing the system daemon for this user.
#
#   make test-e2e
#
# What is verified:
#   1. a client signed as com.sona.test.loopback (standing in for the plug-in host) is accepted;
#   2. the same binary signed as com.sona.test.intruder never gets a hello reply;
#   3. a second host handshake replaces the session and the displaced client reconnects;
#   4. the status listener answers any caller and reports no region stuck in retirement.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
LABEL=com.sona.audio-service
DOMAIN=gui/$(id -u)
PLIST=/tmp/sona-e2e-service.plist
STATE=/tmp/sona-e2e-state.plist
LOG=/tmp/sona-e2e-service.log
BUILD=$ROOT/Driver/build
fail() { echo "FAIL: $*" >&2; exit 1; }

cleanup() {
    launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
    rm -f "$PLIST" "$STATE"
}
trap cleanup EXIT
launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
rm -f "$STATE" "$LOG"

cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$ROOT/Service/build/SonaAudioService</string></array>
  <key>MachServices</key><dict><key>$LABEL</key><true/><key>$LABEL.status</key><true/></dict>
  <key>EnvironmentVariables</key><dict>
    <key>SONA_STATE_FILE</key><string>$STATE</string>
    <key>SONA_PEER_REQUIREMENT</key><string>identifier "com.sona.test.loopback"</string>
  </dict>
  <key>StandardErrorPath</key><string>$LOG</string>
  <key>RunAtLoad</key><true/>
  <key>ThrottleInterval</key><integer>1</integer>
</dict></plist>
EOF
launchctl bootstrap "$DOMAIN" "$PLIST"
sleep 1

echo "== 1. accepted host stand-in"
SONA_LOOPBACK_ITERATIONS=200 "$BUILD/TransportLoopback" || fail "signed loopback client was not accepted"

echo "== 2. rejected intruder (same binary, other signing identifier; waits 5 s for a hello reply)"
if "$BUILD/TransportIntruder" 2>/dev/null; then fail "intruder was accepted"; fi
echo "intruder rejected"

echo "== 3. session replacement"
# Two live "hosts" alternate: each displaced connection reconnects after its backoff and displaces
# the other (newest hello wins). Only one plug-in host exists in production, so this is a test
# artefact; what matters is that the displaced client sees its connection cancelled and comes back.
SONA_LOOPBACK_ITERATIONS=1500 "$BUILD/TransportLoopback" > /tmp/sona-e2e-a.log 2>&1 &
A=$!
sleep 0.7
SONA_LOOPBACK_SECONDS=1 "$BUILD/TransportLoopback2" > /tmp/sona-e2e-b.log 2>&1 || fail "second host handshake failed"
wait "$A" || fail "displaced client did not finish cleanly"
cat /tmp/sona-e2e-a.log
grep -q "reconnected, generation" /tmp/sona-e2e-a.log || fail "displaced client never reconnected"

echo "== 4. status listener"
STATUS=$("$ROOT/Tools/build/sonactl" service)
echo "$STATUS" | grep -q '"retiredRegions" : 0' || fail "a retired region is stuck: $STATUS"
echo "e2e passed"
