#!/bin/sh
# Removes Sona: the menu bar app, the HAL driver and the background service.
#
#   sudo "/Library/Application Support/Sona/uninstall.sh"            keeps settings
#   sudo "/Library/Application Support/Sona/uninstall.sh" --purge    also removes settings
#
# Quit Sona from its menu first so it can hand the system output back to a real device. Even if
# you skip that, macOS picks another output device on its own once the Sona device is gone.
set -u
if [ "$(id -u)" != "0" ]; then echo "run as: sudo \"$0\""; exit 1; fi
LABEL=com.sona.audio-service
SUPPORT="/Library/Application Support/Sona"
console_user=$(stat -f %Su /dev/console 2>/dev/null || echo root)

if pgrep -xq Sona; then
    if [ "$console_user" != "root" ]; then
        launchctl asuser "$(id -u "$console_user")" sudo -u "$console_user" osascript -e 'tell application "Sona" to quit' >/dev/null 2>&1 || true
        sleep 1
    fi
    pkill -x Sona 2>/dev/null || true
fi

launchctl bootout system/$LABEL 2>/dev/null || true
rm -f /Library/LaunchDaemons/$LABEL.plist /Library/PrivilegedHelperTools/SonaAudioService
rm -rf /Library/Audio/Plug-Ins/HAL/SonaDriver.driver
rm -rf /Applications/Sona.app
# coreaudiod restarts without the Sona device; the system falls back to a physical output.
killall coreaudiod 2>/dev/null || true
pkgutil --forget com.sona.pkg >/dev/null 2>&1 || true

if [ "${1:-}" = "--purge" ]; then
    rm -rf "$SUPPORT"
    if [ "$console_user" != "root" ]; then sudo -u "$console_user" defaults delete com.sona.app >/dev/null 2>&1 || true; fi
    echo "Sona 已卸载，设置已清除。"
else
    rm -f "$SUPPORT/uninstall.sh"
    echo "Sona 已卸载。设置保留在 $SUPPORT 和 com.sona.app 偏好中；加 --purge 可一并删除。"
fi
echo "如果声音没有自动恢复，请在「系统设置 → 声音 → 输出」里选择你的扬声器或耳机。"
