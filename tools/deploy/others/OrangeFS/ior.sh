#!/bin/bash

CWD="/home/kfeng/pkg_src/Utility-scripts/OrangeFS"

XSIZES=(32k 128k 512k 2048k 8192k)
PPNS=(40)
IFACE=enp47s0
SERVER_HOSTS=$CWD/servers
CLIENT_HOSTS=$CWD/clients
IOR_BIN=~/pkg_src/ior/src/ior
DEST_FILE=pvfs2://mnt/nvme/kfeng/pvfs2-mount/ior.test
FM=fm
COLL_IO=''
IO_MODE='-w'
REP=1
LOG_FILE=$CWD/ior.log

nserver=`cat $SERVER_HOSTS | wc -l`
nclient=`cat $CLIENT_HOSTS | wc -l`
bsize=32

rm -rf $LOG_FILE
echo "########" >> $LOG_FILE
printf "nserver: %d, nclient: %d\n" $nserver $nclient >> $LOG_FILE
echo "########" >> $LOG_FILE

IO_MODE='-w'
COLL_IO=''
echo "Sequential independent write test"
for ppn in ${PPNS[@]}
do
  for xsize in ${XSIZES[@]}
  do
    echo "Testing xsize=${xsize} ..."
    ((np=$ppn*$nclient))
    xsize_num=`echo $xsize | cut -d'k' -f1`
    echo $xsize_num
    ((nseg=$bsize*1024/$xsize_num))
    for rep in `seq 1 $REP`
    do
      mpssh -f $SERVER_HOSTS "sudo $FM && free -g | grep Mem"
      mpssh -f $CLIENT_HOSTS "sudo $FM && free -g | grep Mem"
      mpiexec -n $np -iface $IFACE -f $CLIENT_HOSTS $IOR_BIN -a MPIIO -t ${xsize} -b ${xsize} -s ${nseg} -H $COLL_IO -E -k $IO_MODE -o $DEST_FILE
      sleep 10
    done
  done
done

sleep 120

IO_MODE='-w'
COLL_IO='-c'
echo "Sequential collective write test"
for ppn in ${PPNS[@]}
do
  for xsize in ${XSIZES[@]}
  do
    echo "Testing xsize=${xsize} ..."
    ((np=$ppn*$nclient))
    xsize_num=`echo $xsize | cut -d'k' -f1`
    echo $xsize_num
    ((nseg=$bsize*1024/$xsize_num))
    for rep in `seq 1 $REP`
    do
      mpssh -f $SERVER_HOSTS "sudo $FM && free -g | grep Mem"
      mpssh -f $CLIENT_HOSTS "sudo $FM && free -g | grep Mem"
      mpiexec -n $np -iface $IFACE -f $CLIENT_HOSTS $IOR_BIN -a MPIIO -t ${xsize} -b ${xsize} -s ${nseg} -H $COLL_IO -E -k $IO_MODE -o $DEST_FILE
      sleep 10
    done
  done
done

sleep 120

IO_MODE='-r'
COLL_IO=''
echo "Sequential independent read test"
for ppn in ${PPNS[@]}
do
  for xsize in ${XSIZES[@]}
  do
    echo "Testing xsize=${xsize} ..."
    ((np=$ppn*$nclient))
    xsize_num=`echo $xsize | cut -d'k' -f1`
    echo $xsize_num
    ((nseg=$bsize*1024/$xsize_num))
    for rep in `seq 1 $REP`
    do
      mpssh -f $SERVER_HOSTS "sudo $FM && free -g | grep Mem"
      mpssh -f $CLIENT_HOSTS "sudo $FM && free -g | grep Mem"
      mpiexec -n $np -iface $IFACE -f $CLIENT_HOSTS $IOR_BIN -a MPIIO -t ${xsize} -b ${xsize} -s ${nseg} -H $COLL_IO -E -k $IO_MODE -o $DEST_FILE
      sleep 10
    done
  done
done

sleep 120

IO_MODE='-r'
COLL_IO='-c'
echo "Sequential collective read test"
for ppn in ${PPNS[@]}
do
  for xsize in ${XSIZES[@]}
  do
    echo "Testing xsize=${xsize} ..."
    ((np=$ppn*$nclient))
    xsize_num=`echo $xsize | cut -d'k' -f1`
    echo $xsize_num
    ((nseg=$bsize*1024/$xsize_num))
    for rep in `seq 1 $REP`
    do
      mpssh -f $SERVER_HOSTS "sudo $FM && free -g | grep Mem"
      mpssh -f $CLIENT_HOSTS "sudo $FM && free -g | grep Mem"
      mpiexec -n $np -iface $IFACE -f $CLIENT_HOSTS $IOR_BIN -a MPIIO -t ${xsize} -b ${xsize} -s ${nseg} -H $COLL_IO -E -k $IO_MODE -o $DEST_FILE
      sleep 10
    done
  done
done
