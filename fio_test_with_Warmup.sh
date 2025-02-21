#!/bin/bash

# Set devices and parameters
devices=("/dev/cas1-1")
iodepths=(1 2 4 8 16 32)
jobnums=(1 2 4 8 16 32)
block_size="4k"
output_file="fio_results_remote_cas.csv"

# Create CSV header
echo "Device,IOdepth,Jobnum,IOPS" > $output_file

# Loop through devices, iodepths, and jobnums
for device in "${devices[@]}"; do
  for iodepth in "${iodepths[@]}"; do
    for jobnum in "${jobnums[@]}"; do
      # First, do a warmup write pass to populate the cache
      warmup_cmd="fio --name=warmup --filename=$device --rw=write --bs=$block_size --direct=1 \
                     --ioengine=libaio --iodepth=32 --size=1G --numjobs=1 --runtime=60 \
                     --group_reporting --output-format=json"
      
      echo "Warming up cache: $warmup_cmd"
      $warmup_cmd

      # Sleep briefly to ensure all writes are completed
      sleep 5

      # Now do the actual read test - it will hit cache since we just wrote this data
      fio_cmd="fio --name=test --filename=$device --rw=randread --bs=$block_size --direct=1 \
               --ioengine=libaio --iodepth=$iodepth --size=1G --time_based --numjobs=$jobnum \
               --runtime=30 --group_reporting --output-format=json"

      # Print fio command for debugging
      echo "Running command: $fio_cmd"

      # Run fio test
      fio_output=$($fio_cmd)
      
      # Parse IOPS from fio output using jq
      iops=$(echo $fio_output | jq '.jobs[] | .read.iops')
      
      # Append results to CSV file
      echo "$device,$iodepth,$jobnum,$iops" >> $output_file
    done
  done
done

echo "FIO tests completed and results saved to $output_file"