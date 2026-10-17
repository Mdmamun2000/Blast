#!/bin/bash
# Run BLAST at several packet-loss rates, record sizes and blast sizes (M);
# collect statistics into results.csv.
# Usage: ./run_experiments.sh <file> "<record sizes>" "<M values>" <runs_per_setting> [loss rates...]
# Example: ./run_experiments.sh test12mb.bin "256 512 1024" "500 2000 10000" 5
#          ./run_experiments.sh test12mb.bin 512 1000 3 0 0.05 0.1
#
# Two machines (the setup the project asks for): build the receiver on the other host,
# make sure "ssh <host>" works without a password, then set
#   RX_SSH=user@10.0.0.2  RX_IP=10.0.0.2  RX_DIR=Blast   ./run_experiments.sh ...
# RX_DIR is the directory (on the receiver host) that holds ./receiver.
# Locally, the receiver runs in RX_DIR (default: a temp directory). Keep it outside folders that
# iCloud/Dropbox sync: they may rename files mid-transfer and break the final rename of <name>.part.

FILE=${1:?usage: $0 <file> "<record sizes>" "<M values>" <runs> [rates...]}
RSS=${2:?record sizes}
MS=${3:?M values}
RUNS=${4:?runs per setting}
RATES=("${@:5}")
[ ${#RATES[@]} -eq 0 ] && RATES=(0 0.01 0.02 0.05 0.10 0.15 0.20 0.30)

PORT=9500
OUT=${OUT:-results.csv}
NAME=$(basename "$FILE")
DEST=recv_$NAME
RX_IP=${RX_IP:-127.0.0.1}
if [ -n "$RX_SSH" ]; then
    RX_DIR=${RX_DIR:-.}; RECEIVER=./receiver
else
    RX_DIR=${RX_DIR:-${TMPDIR:-/tmp}/blast_rx}; RECEIVER="$PWD/receiver"
    mkdir -p "$RX_DIR"
fi

# run a shell command in RX_DIR where the receiver lives (locally, or over ssh)
rx() { if [ -n "$RX_SSH" ]; then ssh "$RX_SSH" "cd $RX_DIR && $1"; else (cd "$RX_DIR" && bash -c "$1"); fi; }

SUM=$(cksum < "$FILE")
echo "loss,record_size,M,run,ok,time_s,throughput_mbps,data_pkts,records_resent,avg_recs_per_pkt,partial_pkts,timeouts,tx_dropped,tx_total,rx_dropped,rx_total" > $OUT

for loss in "${RATES[@]}"; do
    RPID=$(rx "nohup $RECEIVER $PORT 10000 $loss > rx_$loss.log 2>&1 & echo \$!")
    sleep 0.5
    for rs in $RSS; do
    for M in $MS; do
    for run in $(seq 1 $RUNS); do
        rx "rm -f $DEST"
        out=$(./sender $RX_IP $PORT "$FILE" $rs $M $loss $DEST 2>&1)
        sleep 0.3
        [ "$(rx "cksum < $DEST 2>/dev/null")" = "$SUM" ] && ok=1 || ok=0
        if [ $ok = 0 ]; then                    # keep evidence of a failed transfer
            mkdir -p failures
            f=failures/loss${loss}_rs${rs}_M${M}_run${run}
            echo "$out" > $f.sender.log
            rx "ls -la $DEST* 2>&1; cksum < $DEST 2>&1" > $f.receiver.txt
        fi

        tm=$(echo "$out" | sed -n 's/^time \([0-9.]*\) s.*/\1/p')
        tp=$(echo "$out" | sed -n 's/.*throughput \([0-9.]*\) Mbit.*/\1/p')
        pk=$(echo "$out" | sed -n 's/^data packets sent: \([0-9]*\),.*/\1/p')
        rr=$(echo "$out" | sed -n 's/.*records resent: \([0-9]*\).*/\1/p')
        av=$(echo "$out" | sed -n 's/^records per packet: avg \([0-9.]*\).*/\1/p')
        pp=$(echo "$out" | sed -n 's/.*partial packets: \([0-9]*\).*/\1/p')
        to=$(echo "$out" | sed -n 's/.*timeouts: \([0-9]*\).*/\1/p')
        tx=$(echo "$out" | sed -n 's/.*sender side): \([0-9]*\) of \([0-9]*\).*/\1,\2/p')
        rxs=$(rx "grep 'receiver-side garbler dropped' rx_$loss.log | tail -1" \
             | sed -n 's/.*dropped \([0-9]*\) of \([0-9]*\).*/\1,\2/p')

        echo "$loss,$rs,$M,$run,$ok,$tm,$tp,$pk,$rr,$av,$pp,$to,${tx:-0,0},${rxs:-0,0}" >> $OUT
        echo "loss=$loss rs=$rs M=$M run=$run ok=$ok time=${tm}s throughput=${tp} Mbit/s"
    done
    done
    done
    rx "kill $RPID" 2>/dev/null
    rx "cat rx_$loss.log" > rx_$loss.log          # keep a copy of the receiver log here
    sleep 0.3
done
rx "rm -f $DEST"
echo "done -> $OUT"
