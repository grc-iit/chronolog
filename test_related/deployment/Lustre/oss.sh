#!/bin/bash

print_usage() {
  echo "Usage: ./oss.sh [-h] [-m] [-f] [-v] <-n NUM_OST> <-s START_INDEX>"
}

NEED_REFORMAT=""
VERBOSE=""
MOUNT=false

while getopts ":hfmvn:s:" opt; do
  case $opt in
    h)
      print_usage
      exit 0
      ;;
    f)
      NEED_REFORMAT="--reformat"
      ;;
    m)
      MOUNT=true
      ;;
    v)
      VERBOSE="-v"
      ;;
    n)
      NUM_OST=${OPTARG}
      ;;
    s)
      START_INDEX=${OPTARG}
      ;;
    \?)
      echo "Invalid option: -$OPTARG" >&2
      print_usage
      exit 1
      ;;
  esac
done

shift $((OPTIND-1))

if [ -z $NUM_OST ] || [ -z $START_INDEX ]
then
  print_usage
  exit
fi

cyan=`tput setaf 6`
reset=`tput sgr0`

MGSMDS=mgsmds
#NUM_OST=$1 # first arg for number of OST on this OSS
FSNAME=lustre
START_DEV=sde
#START_INDEX=$2 # second arg for the device name of the first OST on this OSS
OST_MOUNT_POINT_BASE=/mnt/ost # OST1 will be mounted at /mnt/ost1, so on and so forth

# Get MGS/MDS ip
mgsmds_ip=$(getent ahosts `cat mgsmds` | grep STREAM | awk '{print $1}')
printf 'MDS/MGS: %s\n' $mgsmds_ip

index=$START_INDEX
device=$START_DEV
for i in `seq 1 $NUM_OST`
do
  if $MOUNT
  then
    # Mount OST
    echo "${cyan}Mounting device /dev/$device at ${OST_MOUNT_POINT_BASE}$index ...${reset}"
    sudo mount $VERBOSE -t $FSNAME /dev/$device ${OST_MOUNT_POINT_BASE}$index
  else
    # Create and format OST
    echo "${cyan}Formatting device /dev/$device as OST-$index ...${reset}"
    sudo mkfs.lustre $VERBOSE --fsname=$FSNAME --ost $NEED_REFORMAT --replace --mgsnode=$mgsmds_ip@tcp --index=$index /dev/$device
    sleep 1
    # Regenerate Lustre configuration logs
    echo "${cyan}Regenerating Lustre configuration logs on device /dev/$device as OST-$index ...${reset}"
    sudo tunefs.lustre --writeconf /dev/$device
    # Create mount points
    sudo mkdir -p ${OST_MOUNT_POINT_BASE}$index
    sudo chown cc:cc ${OST_MOUNT_POINT_BASE}$index
  fi
  # Calculate next device id
  device=$(perl -E "\$device = "$device"; say ++\$device")
  ((index=index+1))
  sleep 2
done
