#!/usr/bin/env bash

#################################################################################
# This script will display a list of notifications using fuzzel and
# then allow the user to select a specific notification and then select an action to
# perform on that notification.
#
# It should be set in the dunstrc config file as the dmenu command.
# dmenu = <path_to_script>/dunst_center.sh
#
# In order to invoke this script, you can use the dunstctl command:
#
# dunstctl context
#
# which you can map to a hotkey in your window manager. e.g. in sway
#
# bindsym $mod+Shift+n exec dunstctl context
#
# This script requires the following packages:
# - fuzzel
# - jq
# - busctl
#
#################################################################################

declare -A entries
declare -A reverse_entries

notifications=$(busctl -j --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.dunstproject.cmd0 -- NotificationListShowing | jq -r '(.data)[][] | "\(.id.data)|\(."summary".data)|\(."appname".data)|\(."body".data)" | gsub("[\\n\\t]"; "")')

if [ -z "$notifications" ]; then
    exit
fi

while IFS=$'\n' read -r line; do
    items=()
    IFS='|' read -ra item <<<"$line"
    for i in "${item[@]}"; do
        items+=("$i")
    done
    display="${items[2]} - ${items[1]}"

    entries["${items[0]}"]="$display"
    reverse_entries["$display"]="${items[0]}"
done <<<"$notifications"

if [ ${#entries[@]} -eq 0 ]; then
    exit
fi

fuzzel_input=""

for key in "${!entries[@]}"; do
    fuzzel_input+="${entries[$key]}\n"
done

fuzzel_output=$(echo -e -n "$fuzzel_input" | fuzzel --output DP-2 -p "select notification:" -d)

if [ -z "$fuzzel_output" ]; then
    exit
fi

notification_id=${reverse_entries[$fuzzel_output]}

declare -a notification_lines
notification_lines=()

readarray -t lines

for line in "${lines[@]}"; do
    if [[ "$line" =~ ^\#([^\(]*)\ \(([^\[]+)\)\ \[(.[0-9]*),(.*)\] ]]; then
        id="${BASH_REMATCH[3]}"

        if [[ "$id" == "$notification_id" ]]; then
            notification_lines+=("$line")
        fi
    fi
done    

fuzzel_choices=""

for line in "${notification_lines[@]}"; do
    if [[ "$line" =~ ^\#([^\(]*)\ \(([^\[]+)\)\ \[(.[0-9]*),(.*)\] ]]; then
        name="${BASH_REMATCH[1]}"
        fuzzel_choices+="$name\n"
    fi
done

fuzzel_output=$(echo -e -n "$fuzzel_choices" | fuzzel --output DP-2 -p "select action:" -d)

if [ -z "$fuzzel_output" ]; then
    exit
fi

for line in "${notification_lines[@]}"; do
  if [[ "$line" =~ \#$fuzzel_output\ \(.*\)\ \[$notification_id,(.*)\] ]]; then
    echo "$line"
  fi
done
