#!/usr/bin/env bash
# iOS proof of concept (branch ios-poc): finds the recompiled function that makes the game crash
# on the iPhone. LIST_FILE holds candidate offsets, comma-separated (for example the functions
# whose generated C differs between two recompilers). Each run keeps half of the candidates
# recompiled and leaves the rest to the interpreter (BB_RECOMP_OFF), then presses Cross until
# gameplay or a crash.
#   BB_IOS_DEVICE=<udid> scripts/ios/bisect.sh LIST_FILE
set -u
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
U=${BB_IOS_DEVICE:?the iPhone UDID (xcrun devicectl list devices)}
A=${BB_IOS_BUNDLE:-io.github.pablovsouza.bloodborne-poc}
mkdir -p out/ios/logs
printf 'show_fps=1\nfps_size=big\nfps_position=bottom_left\noutput_res=960x540\n' > out/ios/logs/bisect.ini
IFS=, read -r -a all < "$1"
cand=("${all[@]}")
step=0
crashes() { # crashes ON...: 0 if the game crashes with only these of the list recompiled
    local on=" $* " off=() f list L=out/ios/logs/bisect-$step.log
    for f in "${all[@]}"; do [[ $on == *" $f "* ]] || off+=("$f"); done
    list=$(IFS=,; echo "${off[*]}")
    xcrun devicectl device copy to --device "$U" --domain-type appDataContainer --domain-identifier "$A" \
        --source out/ios/logs/bisect.ini --destination Documents/bbport.ini >/dev/null 2>&1
    (xcrun devicectl device process launch --device "$U" --terminate-existing --console --environment-variables "{\"BB_IOS_SAVE\":\"mac-save\",\"BB_RECOMP_TARGETS\":\"0\",\"BB_RECOMP_OFF\":\"$list\",\"BB_TEST_TITLE\":\"iOS PoC: bisecting a crash ($step)\",\"BB_TEST_DETAIL\":\"$# of ${#all[@]} functions recompiled; leave the phone\"}" "$A" > "$L" 2>&1 &)
    local end=$((SECONDS + 150)) d pid
    until (( SECONDS > end )) || [[ $(grep -c "Frame stats" "$L" 2>/dev/null) -ge 3 ]]; do sleep 3; done
    for _ in $(seq 1 16); do
        grep -q "Host fault\|terminated" "$L" && break
        BB_IOS_DEVICE=$U bash scripts/ios/press.sh cross 0.3; sleep 5
        d=$(grep "Frame stats" "$L" | tail -1 | grep -o "[0-9]* draws/frame"); [[ ${d%% *} -gt 500 ]] && break
    done
    sleep 5
    local r=1; grep -q "Host fault\|terminated" "$L" && r=0
    pid=$(xcrun devicectl device info processes --device "$U" 2>/dev/null | grep -i BloodbornePoC | awk '{print $1}' | head -1)
    [[ -n $pid ]] && xcrun devicectl device process terminate --device "$U" --pid "$pid" >/dev/null 2>&1
    sleep 3
    return $r
}
while (( ${#cand[@]} > 1 )); do
    half=$(( ${#cand[@]} / 2 )); a=("${cand[@]:0:half}"); b=("${cand[@]:half}")
    step=$((step + 1))
    if crashes "${a[@]}"; then cand=("${a[@]}"); echo "step $step: crash with the first half (${#a[@]})"
    else cand=("${b[@]}"); echo "step $step: no crash with the first half; the second (${#b[@]})"; fi
done
echo "culprit: ${cand[*]}"
step=check; crashes "${cand[@]}" && echo "confirmed: crashes with only ${cand[*]} recompiled" || echo "not confirmed alone"
