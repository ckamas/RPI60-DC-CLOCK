#!/bin/bash
#
# setup.sh - turn a fresh Raspberry Pi OS Lite (Bookworm/Trixie, NetworkManager)
# image into the RPI60-DC-CLOCK appliance:
#
#   * chrony for time sync
#   * lgpio + the clock program, run by clock.service
#   * Comitup: joins known Wi-Fi networks, otherwise runs an access point with
#     a web page for choosing a new network/password
#   * read-only SD card, with a callback that flips it to read-write only while
#     the access point is up (so the new Wi-Fi credentials can be saved), then
#     back to read-only once the Pi is connected again
#
# Run on the Pi, from a checkout of this repo, as root:
#     sudo ./setup.sh [--no-readonly]
# Best run from a console or Ethernet: installing Comitup can briefly
# interrupt a Wi-Fi SSH session.
#
# Optional environment:
#     AP_PASSWORD=...   WPA password (8-63 chars) for the setup access point.
#                       Without it the AP is OPEN - anyone nearby can change the
#                       clock's Wi-Fi.  Strongly recommended.
#     AP_NAME=...       AP/mDNS name (default: rpiclock-<nnn>)
#     COUNTRY=US        Wi-Fi regulatory country (required before Wi-Fi works
#                       on current Pi OS; skip if the Imager already set it)
#
# NOTE: written for a Pi 3 B (on-board 2.4 GHz Wi-Fi, which supports AP mode).
# Boards without on-board Wi-Fi need a USB dongle that supports AP mode
# (check with: iw list | grep -A8 'Supported interface modes').
#
# After this runs, use `sudo rw` / `sudo ro` to open/close the card for edits.

set -euo pipefail

READONLY=1
for a in "$@"; do
    case "$a" in
        --no-readonly) READONLY=0 ;;
        -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

[ "$(id -u)" -eq 0 ] || { echo "Run as root: sudo $0" >&2; exit 1; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$REPO/code/clock.c" ] || { echo "can't find $REPO/code/clock.c" >&2; exit 1; }

AP_NAME="${AP_NAME:-rpiclock-<nnn>}"
AP_PASSWORD="${AP_PASSWORD:-}"
if [ -n "$AP_PASSWORD" ] && { [ "${#AP_PASSWORD}" -lt 8 ] || [ "${#AP_PASSWORD}" -gt 63 ]; }; then
    echo "AP_PASSWORD must be 8-63 characters" >&2; exit 1
fi

say() { printf '\n==> %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }

# Replace (or add) a block delimited by marker comments in a file.
#   put_block FILE TAG  <<'EOF' ... EOF
put_block() {
    local file="$1" tag="$2" tmp
    tmp="$(mktemp)"
    [ -f "$file" ] && sed "/^# BEGIN $tag\$/,/^# END $tag\$/d" "$file" > "$tmp"
    { echo "# BEGIN $tag"; cat; echo "# END $tag"; } >> "$tmp"
    cat "$tmp" > "$file"
    rm -f "$tmp"
}

# ---------------------------------------------------------------- sanity ----
say "Checking system"
. /etc/os-release
echo "OS: ${PRETTY_NAME:-unknown}  arch: $(dpkg --print-architecture)"
case "${VERSION_CODENAME:-}" in
    bookworm|trixie) ;;
    *) warn "tested against bookworm/trixie only (found '${VERSION_CODENAME:-?}')" ;;
esac

# Make sure root and the boot partition are writable while we work
# (harmless on a fresh image; needed when re-running after a previous run).
mount -o remount,rw / || true
BOOTFS=/boot
mountpoint -q /boot/firmware && BOOTFS=/boot/firmware
mount -o remount,rw "$BOOTFS" || true

if ! systemctl list-unit-files NetworkManager.service >/dev/null 2>&1 ||
   ! systemctl list-unit-files NetworkManager.service | grep -q NetworkManager; then
    echo "NetworkManager isn't installed - this script expects a Bookworm/Trixie image." >&2
    exit 1
fi

if ! ls /sys/class/net | grep -q '^wl'; then
    warn "no Wi-Fi interface found right now (is this really a Pi with Wi-Fi, or is"
    warn "the radio rfkill-blocked / country unset?). Continuing anyway."
fi

# -------------------------------------------------------------- packages ----
say "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y gcc make git chrony comitup avahi-daemon rfkill iw

if apt-get install -y liblgpio-dev 2>/dev/null; then
    echo "liblgpio-dev installed from apt"
else
    warn "liblgpio-dev not in apt; building lgpio from source"
    tmp="$(mktemp -d)"
    git clone --depth 1 https://github.com/joan2937/lg.git "$tmp/lg"
    make -C "$tmp/lg"
    make -C "$tmp/lg" install
    ldconfig
    rm -rf "$tmp"
fi

# ----------------------------------------------------------------- chrony ---
say "Time sync: chrony replaces systemd-timesyncd"
systemctl disable --now systemd-timesyncd.service 2>/dev/null || true
systemctl enable chrony.service
# clock.c reads sync state from the kernel (ntp_adjtime). chronyd only clears
# the kernel's unsync flag when rtcsync is on. Debian's default has it; make
# sure (a duplicate directive is harmless).
mkdir -p /etc/chrony/conf.d
printf '# needed by /usr/local/bin/clock\nrtcsync\n' > /etc/chrony/conf.d/rpi60-dc-clock.conf

# --------------------------------------------------- network / Comitup prep --
say "Preparing NetworkManager for Comitup"
systemctl enable NetworkManager.service

# Comitup runs its own dnsmasq for the hotspot; the system one must not grab
# ports 53/67.
systemctl mask dnsmasq.service

# systemd-resolved fights NetworkManager/Comitup for DNS.
if systemctl is-enabled systemd-resolved.service >/dev/null 2>&1; then
    systemctl disable --now systemd-resolved.service
    systemctl mask systemd-resolved.service
    if [ -L /etc/resolv.conf ]; then rm -f /etc/resolv.conf; fi
fi

if grep -Rqs '^\s*dns\s*=\s*dnsmasq' /etc/NetworkManager/NetworkManager.conf /etc/NetworkManager/conf.d 2>/dev/null; then
    warn "'dns=dnsmasq' found in NetworkManager config - Comitup needs it removed"
fi
if [ -f /etc/network/interfaces ] && grep -qE '^\s*(auto|allow-hotplug|iface)\s+wl' /etc/network/interfaces; then
    warn "/etc/network/interfaces references a wifi interface - remove it so NetworkManager can manage Wi-Fi"
fi

# Wi-Fi is soft-blocked until a regulatory country is set.
if [ -n "${COUNTRY:-}" ]; then
    say "Setting Wi-Fi country to $COUNTRY"
    raspi-config nonint do_wifi_country "$COUNTRY"
fi
rfkill unblock wifi || true
iw reg get 2>/dev/null | grep -q 'country 00' &&
    warn "Wi-Fi regulatory country is unset (00). Re-run with COUNTRY=xx or use raspi-config, or the AP may not start."

# ----------------------------------------------------------------- Comitup ---
say "Configuring Comitup"
put_block /etc/comitup.conf "rpi60-dc-clock" <<EOF
ap_name: ${AP_NAME}
$( [ -n "$AP_PASSWORD" ] && echo "ap_password: ${AP_PASSWORD}" || true )
external_callback: /usr/local/bin/comitup-callback
EOF

# ---- read-only <-> read-write helpers --------------------------------------
say "Installing rw/ro helpers and the Comitup callback"
cat > /usr/local/sbin/rw <<EOF
#!/bin/sh
# make the SD card writable
mount -o remount,rw / && mount -o remount,rw $BOOTFS && echo "SD card is now READ-WRITE"
EOF
cat > /usr/local/sbin/ro <<EOF
#!/bin/sh
# make the SD card read-only again
sync
mount -o remount,ro $BOOTFS && mount -o remount,ro / && echo "SD card is now READ-ONLY"
EOF
chmod 755 /usr/local/sbin/rw /usr/local/sbin/ro

# Comitup calls this as root with HOTSPOT | CONNECTING | CONNECTED.
#   HOTSPOT    -> no usable network; the user is about to pick one in the web
#                 page and NetworkManager will save it, so open the card.
#   CONNECTING -> leave as is (profile is being saved/tried).
#   CONNECTED  -> credentials are saved and we're online: close the card.
cat > /usr/local/bin/comitup-callback <<EOF
#!/bin/bash
# installed by RPI60-DC-CLOCK setup.sh
BOOTFS=$BOOTFS
state="\${1:-}"
exec 9>/run/comitup-callback.lock
flock 9

log() { logger -t comitup-callback "\$*"; }

case "\$state" in
    HOTSPOT)
        log "HOTSPOT: remounting SD card read-write"
        mount -o remount,rw / || log "remount / rw FAILED"
        mount -o remount,rw "\$BOOTFS" || log "remount \$BOOTFS rw FAILED"
        ;;
    CONNECTED)
        log "CONNECTED: flushing and remounting SD card read-only"
        sync
        # give NetworkManager a moment to finish writing the profile
        sleep 3
        sync
        for try in 1 2 3 4 5 6; do
            if mount -o remount,ro / ; then
                mount -o remount,ro "\$BOOTFS" || true
                log "SD card is read-only"
                exit 0
            fi
            log "remount ro busy (try \$try), retrying"
            sleep 5
        done
        log "could not remount read-only; leaving SD card read-write"
        ;;
    *)
        : # CONNECTING etc: nothing to do
        ;;
esac
exit 0
EOF
chmod 755 /usr/local/bin/comitup-callback

systemctl enable comitup.service avahi-daemon.service

# ------------------------------------------------------------------ clock ----
say "Building and installing the clock"
gcc -Wall -O2 "$REPO/code/clock.c" -o /usr/local/bin/clock -llgpio
chmod 755 /usr/local/bin/clock

cat > /etc/systemd/system/clock.service <<'EOF'
[Unit]
Description=clock driver for 60-sc display
After=chrony.service

[Service]
ExecStart=/usr/local/bin/clock
Restart=always

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable clock.service

# -------------------------------------------------------------- read-only ----
if [ "$READONLY" -eq 1 ]; then
    say "Making the SD card read-only"

    # Make sure comitup has started once while still writable so it can create
    # its persistent state (e.g. the <nnn> in the AP name). Plain "start" - a
    # restart could drop the Wi-Fi link you are running this over.
    systemctl start comitup.service || true
    sleep 5

    cp -n /etc/fstab /etc/fstab.orig
    cp -n "$BOOTFS/cmdline.txt" "$BOOTFS/cmdline.txt.orig"

    # add ro (and noatime on /) to the root and boot entries in fstab
    awk '
        /^[[:space:]]*#/ || NF < 4 { print; next }
        ($2 == "/" || $2 == "/boot" || $2 == "/boot/firmware") {
            if ($4 !~ /(^|,)ro(,|$)/) $4 = $4 ",ro"
            if ($2 == "/" && $4 !~ /noatime/) $4 = $4 ",noatime"
        }
        { print }
    ' /etc/fstab.orig > /etc/fstab.new
    cat /etc/fstab.new > /etc/fstab && rm -f /etc/fstab.new

    # kernel command line must be ONE line; add ro + fsck.repair if missing
    cl="$(tr -d '\n' < "$BOOTFS/cmdline.txt")"
    case " $cl " in *" ro "*) ;; *) cl="$cl ro" ;; esac
    case " $cl " in *" fsck.repair="*) ;; *) cl="$cl fsck.repair=yes" ;; esac
    printf '%s\n' "$cl" > "$BOOTFS/cmdline.txt"

    # Volatile directories that programs expect to be writable.
    chrony_ids=""
    if id -u _chrony >/dev/null 2>&1; then
        chrony_ids=",uid=$(id -u _chrony),gid=$(id -g _chrony)"
    fi
    put_block /etc/fstab "rpi60-dc-clock-tmpfs" <<EOF
tmpfs /tmp                  tmpfs nodev,nosuid 0 0
tmpfs /var/tmp              tmpfs nodev,nosuid 0 0
tmpfs /var/log              tmpfs nodev,nosuid,mode=0755 0 0
tmpfs /var/lib/NetworkManager tmpfs nodev,nosuid,mode=0755 0 0
tmpfs /var/lib/chrony       tmpfs nodev,nosuid,mode=0750${chrony_ids} 0 0
EOF

    mkdir -p /etc/systemd/journald.conf.d
    printf '[Journal]\nStorage=volatile\n' > /etc/systemd/journald.conf.d/volatile.conf

    echo "fstab and cmdline.txt updated (originals saved as *.orig)"
fi

# ------------------------------------------------------------------ done -----
say "Done"
cat <<EOF
Reboot to finish:  sudo reboot

Afterwards:
  * Known Wi-Fi  -> the Pi joins it, clock runs, card stays read-only.
  * No known Wi-Fi -> access point '${AP_NAME}' appears. Join it, open
    http://10.41.0.1/ (or the captive-portal page), pick a network and enter
    the password.  The card is read-write only while the AP is up and goes
    back to read-only when the Pi connects.
  * To change anything on the card by hand:  sudo rw  ...  sudo ro
  * Logs: journalctl -t comitup-callback ; /var/log/comitup.log
EOF
