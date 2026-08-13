#!/usr/bin/env bash
# End-user setup script for a regional gpsxdb DB host -- NOT the full
# master/planet server (that's a separate, much heavier setup). This is
# for someone who will separately obtain a <region>.gpsxdb.tar.gz bundle
# and install it themselves; a Raspberry Pi 5 (with an NVMe SSD) is an
# explicitly supported target for exactly this scenario (see README.md's
# hardware notes).
#
# What this script does:
#   - installs OS packages, clones/pulls the repo, builds all binaries
#   - creates a local Postgres role + database with the postgis extension
#     enabled (the one schema step NOT self-provisioned by the code --
#     everything else, including postgis_raster and every table, is
#     created automatically the first time regional_install runs)
#   - installs a systemd unit for region-aware poll
#
# What this script deliberately does NOT do:
#   - download or install any region bundle
#   - run regional_install
#   - enable or start the poll service
# Those are manual steps you do yourself once you have a bundle -- see the
# instructions this script prints at the end.
#
# Usage: sudo DB_NAME=myregion ./setup_regional_server.sh
# Every variable below can be overridden via environment variable.
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/dmars1972/gpsxdb.git}"
INSTALL_DIR="${INSTALL_DIR:-/opt/gpsxdb}"
SERVICE_USER="${SERVICE_USER:-gpsxdb}"
DB_NAME="${DB_NAME:-}"
DB_ROLE="${DB_ROLE:-gpsxdb_svc}"
DB_HOST="${DB_HOST:-localhost}"
DB_PASSWORD="${DB_PASSWORD:-}"

log() { echo "[setup_regional_server] $*"; }

# ---- Phase 1: preflight ----
preflight() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "This script must be run as root (e.g. sudo ./setup_regional_server.sh)" >&2
        exit 1
    fi
    if [ -z "$DB_NAME" ]; then
        echo "DB_NAME must be set, e.g.: sudo DB_NAME=myregion ./setup_regional_server.sh" >&2
        exit 1
    fi
    if [ -f /etc/os-release ] && ! grep -qiE "ubuntu|debian" /etc/os-release; then
        log "warning: this script is written for Ubuntu/Debian -- proceeding anyway, but package names may not match"
    fi
}

# ---- Phase 2: OS packages ----
# Full dependency set is required even though regional_install itself only
# needs libpqxx/libpq -- region-aware poll runs via the osm_import binary,
# which is one monolithic CMake target that always links protobuf/PROJ/
# curl/lz4/expat regardless of which mode you run it in. No trimmed-down
# build target exists. libcurl4-openssl-dev is required by CMakeLists.txt
# but missing from the README's own apt list -- included here.
install_packages() {
    log "installing OS packages..."
    apt update
    apt install -y \
        build-essential cmake git \
        libboost-program-options-dev \
        libexpat1-dev zlib1g-dev \
        libpqxx-dev libpq-dev libproj-dev \
        protobuf-compiler libprotobuf-dev \
        liblz4-dev libcurl4-openssl-dev \
        postgresql postgresql-contrib postgis
}

# ---- Phase 3: service account ----
create_service_account() {
    if id -u "$SERVICE_USER" &>/dev/null; then
        log "service account $SERVICE_USER already exists"
    else
        log "creating service account $SERVICE_USER..."
        useradd --system --create-home --shell /usr/sbin/nologin "$SERVICE_USER"
    fi
}

# ---- Phase 4: clone/pull + build ----
clone_and_build() {
    if [ -d "$INSTALL_DIR/.git" ]; then
        log "pulling latest into $INSTALL_DIR..."
        # Run as $SERVICE_USER, not root: the directory is owned by
        # $SERVICE_USER (chown'd below on first install), and modern git
        # refuses to operate on a repository it doesn't own ("detected
        # dubious ownership") when run as a different user -- confirmed
        # live on a re-run against an already-installed box.
        chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR"
        sudo -u "$SERVICE_USER" git -C "$INSTALL_DIR" pull
    else
        log "cloning into $INSTALL_DIR..."
        # Must run as root here: /opt is not writable by $SERVICE_USER, so
        # only root can create the initial directory. Ownership is handed
        # to $SERVICE_USER immediately after.
        git clone "$REPO_URL" "$INSTALL_DIR"
        chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR"
    fi

    log "building (this can take a while)..."
    # -j$(nproc) can OOM-kill cc1plus on RAM-constrained boxes (Boost.Geometry
    # translation units + pqxx headers are memory-heavy) -- if this fails,
    # re-run manually with a lower -j, e.g.:
    #   sudo -u SERVICE_USER bash -c "cd INSTALL_DIR && cmake --build build -j4"
    sudo -u "$SERVICE_USER" bash -c "cd '$INSTALL_DIR' && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
}

# ---- Phase 5: database + postgis extension + role ----
# postgis_raster and every table are self-provisioned by regional_install's
# NavDB::ensureSchema() the first time it runs -- deliberately not
# duplicated here. No custom data_directory, no tuning: target hardware is
# unknown (anywhere from a Raspberry Pi 5 to a desktop), so apt defaults
# are the safe choice. See README.md's tuning table for optional manual
# follow-up if you want to optimize for your specific box.
setup_database() {
    if [ -z "$DB_PASSWORD" ]; then
        DB_PASSWORD="$(openssl rand -base64 24)"
        log "generated a new password for role $DB_ROLE"
    fi

    log "creating role $DB_ROLE (if not already present)..."
    sudo -u postgres psql -v ON_ERROR_STOP=1 -c "
        DO \$\$ BEGIN
            IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '$DB_ROLE') THEN
                CREATE ROLE $DB_ROLE LOGIN PASSWORD '$DB_PASSWORD';
            END IF;
        END \$\$;"

    log "creating database $DB_NAME (if not already present)..."
    sudo -u postgres createdb -O "$DB_ROLE" "$DB_NAME" 2>/dev/null || log "database $DB_NAME already exists, skipping"

    log "enabling postgis extension..."
    sudo -u postgres psql -v ON_ERROR_STOP=1 -d "$DB_NAME" -c "CREATE EXTENSION IF NOT EXISTS postgis;"
}

# ---- Phase 6: pg_hba.conf + .pgpass ----
# Local-only by default (loopback TCP + peer for the Unix socket) -- this
# targets a single end-user machine, not a shared server, so there's no
# CIDR to guess at.
configure_auth() {
    local pg_hba
    pg_hba="$(find /etc/postgresql -maxdepth 3 -name pg_hba.conf | head -1)"
    if [ -z "$pg_hba" ]; then
        log "warning: could not find pg_hba.conf, skipping auth config -- add entries manually"
    else
        local marker="# gpsxdb regional install ($DB_NAME/$DB_ROLE)"
        if grep -qF "$marker" "$pg_hba" 2>/dev/null; then
            log "pg_hba.conf entries already present, skipping"
        else
            log "adding pg_hba.conf entries for $DB_ROLE..."
            {
                echo "$marker"
                echo "host    $DB_NAME    $DB_ROLE    127.0.0.1/32    scram-sha-256"
                echo "host    $DB_NAME    $DB_ROLE    ::1/128         scram-sha-256"
            } >> "$pg_hba"
            systemctl reload postgresql
        fi
    fi

    local pgpass="/home/$SERVICE_USER/.pgpass"
    if sudo -u "$SERVICE_USER" test -f "$pgpass"; then
        log ".pgpass already exists for $SERVICE_USER, leaving it untouched"
    else
        log "writing .pgpass for $SERVICE_USER..."
        sudo -u "$SERVICE_USER" bash -c "echo '$DB_HOST:5432:$DB_NAME:$DB_ROLE:$DB_PASSWORD' > '$pgpass' && chmod 600 '$pgpass'"
    fi
}

# ---- Phase 7: region-aware poll systemd unit (installed, NOT enabled) ----
# Deliberately not enabled or started: there's no --nodes-file for it to
# poll against until you've manually run regional_install. Auto-starting
# (even just "enabled" for next boot) would just crash-loop.
install_poll_service() {
    local unit_path="/etc/systemd/system/gpsxdb-regional-poll.service"
    local unit_content
    unit_content="$(cat <<EOF
[Unit]
Description=gpsxdb regional OSM replication poll (minutely)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
Environment=HOME=/home/$SERVICE_USER
WorkingDirectory=$INSTALL_DIR
# EDIT THIS PATH before enabling -- it must exactly match whatever
# --nodes-file path you pass to \`regional_install\` when you install your
# region bundle. This placeholder will not work as-is.
ExecStart=$INSTALL_DIR/build/osm_import -m poll -r minute -s $DB_HOST -d $DB_NAME -u $DB_ROLE -f $INSTALL_DIR/CHANGE_ME_region.nodes.dat
Restart=always
RestartSec=30
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
)"
    if [ -f "$unit_path" ] && [ "$(cat "$unit_path")" = "$unit_content" ]; then
        log "systemd unit already up to date, leaving it untouched"
    else
        log "installing systemd unit at $unit_path (not enabling -- see script header)..."
        echo "$unit_content" > "$unit_path"
        systemctl daemon-reload
    fi
}

print_final_instructions() {
    cat <<EOF

============================================================
Setup complete.

  - Postgres role:      $DB_ROLE
  - Database:            $DB_NAME (postgis extension enabled)
  - Code built at:        $INSTALL_DIR/build
  - Service account:      $SERVICE_USER

Remaining steps (manual, once you have a region bundle):

  1. Install the bundle:

       sudo -u $SERVICE_USER $INSTALL_DIR/build/regional_install \\
         <region>.gpsxdb.tar.gz -s $DB_HOST -d $DB_NAME -u $DB_ROLE \\
         --nodes-file $INSTALL_DIR/<region>.nodes.dat

  2. Edit /etc/systemd/system/gpsxdb-regional-poll.service's ExecStart --
     replace CHANGE_ME_region.nodes.dat with the exact --nodes-file path
     you used above.

  3. Enable and start the poll service:

       sudo systemctl enable --now gpsxdb-regional-poll
============================================================
EOF
}

main() {
    preflight
    install_packages
    create_service_account
    clone_and_build
    setup_database
    configure_auth
    install_poll_service
    print_final_instructions
}

main "$@"
