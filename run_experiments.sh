#!/bin/bash
# Run BLAST at several packet-loss rates and collect statistics into results.csv.
# Usage: ./run_experiments.sh <file> <record_size> <M> <runs_per_rate> [loss rates...]
# Example: ./run_experiments.sh test10mb.bin 512 1000 5
#          ./run_experiments.sh test10mb.bin 512 1000 3 0 0.05 0.1

FILE=${1:?usage: $0 <file> <record_size> <M> <runs> [rates...]}
RS=${2:?record size}
M=${3:?M}
RUNS=${4:?runs per rate}
RATES=("${@:5}")
[ ${#RATES[@]} -eq 0 ] && RATES=(0 0.01 0.02 0.05 0.10 0.15 0.20 0.30)

PORT=9500
OUT=results.csv
echo "loss,run,ok,time_s,throughput_mbps,data_pkts,records_resent,timeouts,tx_dropped,tx_total,rx_dropped,rx_total" > $OUT

for loss in "${RATES[@]}"; do
    ./receiver $PORT 10000 $loss > rx_$loss.log 2>&1 &
    RPID=$!
    sleep 0.5
    for run in $(seq 1 $RUNS); do
        rm -f recv_$(basename $FILE)
        out=$(./sender 127.0.0.1 $PORT $FILE $RS $M $loss 2>&1)
        sleep 0.3
        cmp -s $FILE recv_$(basename $FILE) && ok=1 || ok=0

        tm=$(echo "$out" | sed -n 's/^time \([0-9.]*\) s.*/\1/p')
        tp=$(echo "$out" | sed -n 's/.*throughput \([0-9.]*\) Mbit.*/\1/p')
        pk=$(echo "$out" | sed -n 's/^data packets sent: \([0-9]*\),.*/\1/p')
        rr=$(echo "$out" | sed -n 's/.*records resent: \([0-9]*\).*/\1/p')
        to=$(echo "$out" | sed -n 's/.*timeouts: \([0-9]*\).*/\1/p')
        tx=$(echo "$out" | sed -n 's/.*sender side): \([0-9]*\) of \([0-9]*\).*/\1,\2/p')
        rx=$(grep 'receiver-side garbler dropped' rx_$loss.log | tail -1 \
             | sed -n 's/.*dropped \([0-9]*\) of \([0-9]*\).*/\1,\2/p')

        echo "$loss,$run,$ok,$tm,$tp,$pk,$rr,$to,${tx:-0,0},${rx:-0,0}" >> $OUT
        echo "loss=$loss run=$run ok=$ok time=${tm}s throughput=${tp} Mbit/s"
    done
    kill $RPID 2>/dev/null
    wait $RPID 2>/dev/null
done
rm -f recv_$(basename $FILE)
echo "done -> $OUT"
