#!/bin/bash
# Configure ChickenBlocker's read-only Google Calendar OAuth synchronization.
set -euo pipefail

if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != "--oauth" ]; }; then
  echo "Usage: $0 [--oauth]" >&2
  exit 2
fi

if ! command -v vdirsyncer >/dev/null 2>&1 \
    || ! python3 -c 'import aiohttp_oauthlib' >/dev/null 2>&1; then
  echo "OAuth support requires vdirsyncer and python3-aiohttp-oauthlib." >&2
  echo "Install them with: sudo apt install vdirsyncer python3-aiohttp-oauthlib" >&2
  exit 1
fi

config_dir="$HOME/.config/chickenblocker"
data_dir="$HOME/.local/share/chickenblocker"
cache_dir="$HOME/.cache/chickenblocker"
calendar_dir="$data_dir/calendar"
status_dir="$data_dir/vdirsyncer-status"
token_file="$config_dir/google-oauth-token.json"

printf '%s\n' \
  'Create the Google OAuth application before continuing:' \
  '  1. Open https://console.cloud.google.com/ and create/select a project.' \
  '  2. Open Google Auth Platform -> Branding and click Get started.' \
  '  3. Enter an app name (for example ChickenBlocker Calendar), support email,' \
  '     and developer contact email.' \
  '  4. Choose External if this project is owned by a personal account.' \
  '     Choose Internal only if the project belongs to the company organization.' \
  '  5. For External: open Audience -> Test users and add the corporate email.' \
  '  6. For External: open Data Access -> Add or remove scopes and add:' \
  '       https://www.googleapis.com/auth/calendar' \
  '  7. In APIs & Services -> Library, enable Google CalDAV API.' \
  '  8. Open Google Auth Platform -> Clients -> Create client -> Desktop app.' \
  '  9. Copy the client ID and client secret and enter them below.' \
  '' \
  'External apps in Testing expire authorization and refresh tokens after 7 days.' \
  'Publishing the app removes that Testing expiry but can show an unverified-app warning.' \
  'Your Workspace administrator may need to allow this OAuth client.' \
  'Vdirsyncer requests Google Calendar access, but this configuration never uploads changes.' \
  ''

read -r -p 'Google account / primary calendar email: ' account
read -r -p 'Desktop OAuth client ID: ' client_id
read -r -s -p 'Desktop OAuth client secret: ' client_secret
printf '\n'

account_pattern='^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$'
client_id_pattern='^[A-Za-z0-9._-]+\.apps\.googleusercontent\.com$'
secret_pattern='^[A-Za-z0-9._-]+$'
if [[ ! $account =~ $account_pattern ]]; then
  echo "Invalid Google account email." >&2
  exit 2
fi
if [[ ! $client_id =~ $client_id_pattern ]]; then
  echo "Invalid Desktop OAuth client ID." >&2
  exit 2
fi
if [[ ! $client_secret =~ $secret_pattern ]]; then
  echo "Invalid Desktop OAuth client secret." >&2
  exit 2
fi

umask 077
mkdir -p "$config_dir" "$calendar_dir/primary" "$status_dir" "$cache_dir"

vdir_tmp="$config_dir/.vdirsyncer.conf.tmp.$$"
khal_tmp="$config_dir/.khal.conf.tmp.$$"
trap 'rm -f "$vdir_tmp" "$khal_tmp"' EXIT

printf '%s\n' \
  '[general]' \
  "status_path = \"$status_dir\"" \
  '' \
  '[pair chickenblocker]' \
  'a = "calendar_local"' \
  'b = "calendar_remote"' \
  "collections = [[\"primary\", \"primary\", \"$account\"]]" \
  'conflict_resolution = "b wins"' \
  'partial_sync = "revert"' \
  '' \
  '[storage calendar_local]' \
  'type = "filesystem"' \
  "path = \"$calendar_dir\"" \
  'fileext = ".ics"' \
  '' \
  '[storage calendar_remote]' \
  'type = "google_calendar"' \
  'read_only = true' \
  'item_types = ["VEVENT"]' \
  'start_date = "datetime.now() - timedelta(days=1)"' \
  'end_date = "datetime.now() + timedelta(days=31)"' \
  "token_file = \"$token_file\"" \
  "client_id = \"$client_id\"" \
  "client_secret = \"$client_secret\"" > "$vdir_tmp"

printf '%s\n' \
  '[calendars]' \
  '[[chickenblocker]]' \
  "path = $calendar_dir/primary" \
  'readonly = True' \
  '' \
  '[locale]' \
  'timeformat = %H:%M' \
  'dateformat = %Y-%m-%d' \
  'longdateformat = %Y-%m-%d' \
  'datetimeformat = %Y-%m-%dT%H:%M:%S%z' \
  'longdatetimeformat = %Y-%m-%dT%H:%M:%S%z' \
  '' \
  '[sqlite]' \
  "path = $cache_dir/khal.db" > "$khal_tmp"

chmod 0600 "$vdir_tmp" "$khal_tmp"
mv -f "$vdir_tmp" "$config_dir/vdirsyncer.conf"
mv -f "$khal_tmp" "$config_dir/khal.conf"
trap - EXIT

echo "Opening Google authorization in your browser..."
if [ "${CHICKENBLOCKER_SKIP_OAUTH_LOGIN:-0}" != "1" ]; then
  # A token is bound to the previous client/account; configuration is an
  # explicit reauthorization operation.
  rm -f "$token_file"
  if ! vdirsyncer --config "$config_dir/vdirsyncer.conf" discover chickenblocker; then
    echo "Google authorization or calendar discovery failed." >&2
    echo "The configuration was kept so the command can be retried." >&2
    exit 1
  fi
  [ ! -e "$token_file" ] || chmod 0600 "$token_file"
fi

echo "OAuth calendar synchronization configured."
echo "The first event sync will run from ChickenBlocker during an eligible work interval."
echo "No manual vdirsyncer or khal commands are required."
