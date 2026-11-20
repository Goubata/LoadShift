SKIPUNZIP=0
if [ "$ARCH" != "arm64" ]; then abort "arm64 only"; fi
mkdir -p /data/adb/loadshift/profiles
chmod 700 /data/adb/loadshift /data/adb/loadshift/profiles
set_perm_recursive "$MODPATH" 0 0 0755 0644
ui_print "- LoadShift: configure apps in the LoadShift app, then reboot"
