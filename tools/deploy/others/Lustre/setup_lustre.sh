#!/bin/bash

source ~/.bashrc

red=`tput setaf 1`
green=`tput setaf 2`
cyan=`tput setaf 6`
reset=`tput sgr0`

MGSMDS=mgsmds
OSS=oss
CLIENTS=clients
NUM_OST=32
NUM_OSS=$(cat $OSS | wc -l)
FSNAME=lustre
START_DEV=/dev/sde
MGS_DEV=/dev/sdp
MDS_DEV=/dev/sdo
CLIENT_MOUNT_POINT=/mnt/lustre
MGT_MOUNT_POINT=/mnt/mgt
MDT_MOUNT_POINT=/mnt/mdt
OST_MOUNT_POINT_BASE=/mnt/ost # OST1 will be mounted at /mnt/ost1, so on and so forth
PWD=$(pwd)

#sync_file 1> /dev/null 2>&1
#if [[ $? != 0 ]]
#then
  #echo "${red}sync_file command is not found, exiting ...${reset}"
  #exit 1
#fi

#sync_file -q $MGSMDS
#sync_file -q $OSS
#sync_file -q $CLIENTS
#sync_file -q oss.sh

# Unmount clients
echo "${cyan}Unmounting clients ...${reset}"
mpssh -f $CLIENTS "sudo umount $CLIENT_MOUNT_POINT"

# Unmount MGT/MDT
echo "${cyan}Unmounting MGT ...${reset}"
mpssh -f $MGSMDS "sudo umount $MGT_MOUNT_POINT"
echo "${cyan}Unmounting MDT ...${reset}"
mpssh -f $MGSMDS "sudo umount $MDT_MOUNT_POINT"

# Unmount OST
echo "${cyan}Unmounting OSTs ...${reset}"
mpssh -f $OSS "for mount_point in \`ls -d ${OST_MOUNT_POINT_BASE}*\`; do echo sudo umount \$mount_point; done; echo sudo rm -rf ${OST_MOUNT_POINT_BASE}*"

# Clean up RAID configuration from previous users
echo "${cyan}Stopping RAID configuration ...${reset}"
mpssh -f $MGSMDS "if ls /dev/md[0-9]* 1> /dev/null 2>&1; then sudo mdadm --stop /dev/md[0-9]*; fi"
mpssh -f $OSS "if ls /dev/md[0-9]* 1> /dev/null 2>&1; then sudo mdadm --stop /dev/md[0-9]*; fi"

# Checking LVM configuration from previous users
echo "${cyan}Checking if LVM is configured ...${reset}"
if [[ $(mpssh -f $MGSMDS "sudo vgdisplay" | grep "\->") ]]
then
  echo "${red}MGS/MDS has LVM configured, fix it before running this script again, exiting ...${reset}"
  exit 1
fi
if [[ $(mpssh -f $OSS "sudo vgdisplay" | grep "\->") ]]
then
  echo "${red}OSS has LVM configured, fix it before running this script again, exiting ...${reset}"
  exit 1
fi
echo "${green}LVM is not found on MGS/MDS or OSS, moving forward ...${reset}"

# Get MGS/MDS ip
mgsmds_ip=$(getent ahosts `cat mgsmds` | grep STREAM | awk '{print $1}')
echo "${cyan}The IP of MGS is $mgsmds_ip ${reset}"

# Configure MGS/MDS
mpssh -f $MGSMDS "sudo mkfs.lustre --reformat --mgs $MGS_DEV;
                  sudo mkfs.lustre --fsname=$FSNAME --reformat --replace --mgsnode=$mgsmds_ip@tcp --mdt --index=0 $MDS_DEV;
                  sudo mkdir -p $MDT_MOUNT_POINT;
                  sudo mkdir -p $MGT_MOUNT_POINT"
                  #sudo tunefs.lustre --writeconf $MDS_DEV"

# Configure OSS
((num_ost_per_node=$NUM_OST/$NUM_OSS))
start_index=0
for node in $(cat $OSS | awk '{print $1}')
do
  echo "${green}Running oss.sh on OSS $node with $num_ost_per_node OSTs starting from $start_index ... ${reset}"
  ssh $node "cd $PWD; ./oss.sh -f -n $num_ost_per_node -s $start_index"
  ((start_index+=$num_ost_per_node))
  sleep 5
done

# Mount on MGS/MDS
mpssh -f $MGSMDS "sudo mount -t lustre $MGS_DEV $MGT_MOUNT_POINT;
                  sudo mount -t lustre $MDS_DEV $MDT_MOUNT_POINT"

# Mount on OSS
start_index=0
for node in $(cat $OSS | awk '{print $1}')
do
  echo "${green}Running oss.sh on OSS $node with $num_ost_per_node OSTs starting from $start_index ... ${reset}"
  ssh $node "cd $PWD; ./oss.sh -m -n $num_ost_per_node -s $start_index"
  ((start_index+=$num_ost_per_node))
  sleep 5
done

# Mount on clients
mpssh -f $CLIENTS "sudo mkdir -p $CLIENT_MOUNT_POINT;
                   sudo mount -t lustre $mgsmds_ip@tcp:/$FSNAME $CLIENT_MOUNT_POINT;
                   sudo chown -R cc:cc $CLIENT_MOUNT_POINT;
                   lfs setstripe -c $NUM_OST $CLIENT_MOUNT_POINT"
