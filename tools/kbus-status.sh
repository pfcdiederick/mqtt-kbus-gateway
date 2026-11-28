#!/usr/bin/env bash
# KBUS timing (latency), health and load of kbus-gateway on a PFC, and its hang diagnosis.
#
#   tools/kbus-status.sh <pfc-ip>            timing, health, memory and CPU (measured over 10 s)
#   tools/kbus-status.sh <pfc-ip> --stall    the hang diagnosis files (kbus-stall-*.txt), newest in full
#
# Read only: nothing is changed on the controller. Works with the container and the
# native package (the broker clients of the kbus-mosquitto container, or mosquitto_sub).
set -euo pipefail
PFC=${1:?usage: $0 <pfc-ip> [--stall]}
MODE=${2:-}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 ${PFC_USER:-root}@$PFC"

if [[ $MODE == --stall ]]; then
    $SSH 'ls -t /home/kbus-gateway/kbus-stall-*.txt 2>/dev/null > /tmp/.stall || true
          n=$(wc -l < /tmp/.stall)
          if [ "$n" = 0 ]; then echo "no hang diagnosis files: the KBUS cycle has not hung (kbus-gateway >= 3.4.1)"; exit 0; fi
          echo "$n hang diagnosis file(s) in /home/kbus-gateway:"; sed "s|.*/|  |" /tmp/.stall
          echo; echo "=== newest: $(head -1 /tmp/.stall)"; cat "$(head -1 /tmp/.stall)"'
    exit 0
fi

$SSH 'sub() { if docker exec mosquitto true 2>/dev/null; then docker exec mosquitto mosquitto_sub "$@"; else mosquitto_sub "$@"; fi; }
      s=$(sub -t kbus/bus/statistics -C 1 -W 15 2>/dev/null || true)
      [ -n "$s" ] || { echo "no kbus/bus/statistics on the broker: is kbus-gateway running?"; exit 1; }
      v() { echo "$s" | sed -n "s/.*\"$1\":\"\{0,1\}\([^,\"}]*\).*/\1/p"; }
      us() { awk -v x="$1" "BEGIN{printf \"%.2f ms\", x/1000}"; }
      echo "controller   $(sed -n "s/^FIRMWARE=//p" /etc/REVISIONS), $(uname -m), $(grep -c ^processor /proc/cpuinfo) CPU core(s)"
      echo "gateway      $(v version)  healthy=$(v healthy)  realtime=$(v realtime)"
      echo "modules      $(sub -t kbus/modules -C 1 -W 3 2>/dev/null | grep -o "\"name\":\"[^\"]*\"" | cut -d\" -f4 | tr "\n" "," | sed "s/,$//")"
      echo
      echo "KBUS timing (since the start or the last Reset Statistics)"
      echo "  cycle time               $(v cycleTime) ms"
      echo "  exchange, last           $(us $(v cycleDuration))      (write outputs + push + read inputs)"
      echo "  exchange, longest        $(us $(v maxCycleDuration))      (keep well below the cycle time)"
      echo "  wake-up delay, longest   $(us $(v maxWakeupDelay))      (other real-time threads, e.g. CODESYS)"
      echo "  cycles / errors / overruns  $(v cycles) / $(v errors) / $(v overruns)"
      p=$(pidof kbus-gateway || true)
      if [ -n "$p" ]; then
          a=$(awk "{print \$14+\$15}" /proc/$p/stat); sleep 10; b=$(awk "{print \$14+\$15}" /proc/$p/stat)
          echo
          echo "load of kbus-gateway (pid $p, running $(ps -o etime= -p $p | tr -d " "))"
          echo "  CPU                      $(awk -v d=$((b-a)) "BEGIN{printf \"%.1f\", d/10}") % of one core (measured over 10 s)"
          echo "  memory                   $(grep VmRSS /proc/$p/status | awk "{print \$2}") kB resident, peak $(grep VmHWM /proc/$p/status | awk "{print \$2}") kB"
          echo "  system load              $(cut -d" " -f1-3 /proc/loadavg)"
      fi
      n=$(ls /home/kbus-gateway/kbus-stall-*.txt 2>/dev/null | wc -l)
      echo
      [ "$n" = 0 ] && echo "hang diagnosis: none" || echo "hang diagnosis: $n file(s), see: tools/kbus-status.sh <pfc-ip> --stall"'
