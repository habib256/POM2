#!/usr/bin/env bash
# Listen to floppy mechanical samples one by one.
# Usage:  ./play.sh           interactive menu
#         ./play.sh 525       only 5.25" samples
#         ./play.sh 35        only 3.5" samples
#         ./play.sh all       play everything in order

set -u
cd "$(dirname "$0")"

filter="${1:-menu}"

case "$filter" in
    525) mapfile -t files < <(ls 525_*.wav | sort) ;;
    35)  mapfile -t files < <(ls 35_*.wav  | sort) ;;
    all) mapfile -t files < <(ls *.wav     | sort) ;;
    menu|"")
        mapfile -t files < <(ls *.wav | sort)
        while true; do
            echo
            echo "=== Floppy samples ==="
            for i in "${!files[@]}"; do
                printf "  %2d) %s\n" "$((i+1))" "${files[$i]}"
            done
            echo "   q) quit"
            read -rp "> " choice
            [[ "$choice" == "q" ]] && exit 0
            [[ "$choice" =~ ^[0-9]+$ ]] || continue
            idx=$((choice-1))
            [[ $idx -ge 0 && $idx -lt ${#files[@]} ]] || continue
            echo "▶ ${files[$idx]}"
            aplay -q "${files[$idx]}"
        done
        ;;
    *)
        echo "Unknown filter: $filter (use: 525 | 35 | all | menu)" >&2
        exit 2
        ;;
esac

for f in "${files[@]}"; do
    echo "▶ $f"
    aplay -q "$f"
    sleep 0.3
done
