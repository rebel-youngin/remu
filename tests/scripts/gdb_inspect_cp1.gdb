# Pure-GDB (no Python) variant of the CP1 bring-up verification script.
# Assumes QEMU is already running with -s (gdbstub on :1234) and has had
# enough wall-clock time to reach FreeRTOS on every chiplet; attach pauses
# the VM, and we dump per-thread frames for every CP1 vCPU.
#
# Thread layout (from docs/debugging.md):
#   1..4  = chiplet 0 CP0.cpu0..3      5..8  = chiplet 0 CP1.cpu0..3
#   9..12 = chiplet 1 CP0.cpu0..3     13..16 = chiplet 1 CP1.cpu0..3
#  17..20 = chiplet 2 CP0.cpu0..3     21..24 = chiplet 2 CP1.cpu0..3
#  25..28 = chiplet 3 CP0.cpu0..3     29..32 = chiplet 3 CP1.cpu0..3
#
# Usage:
#   aarch64-none-elf-gdb -batch \
#     -ex 'target remote :1234' \
#     -x tests/scripts/gdb_inspect_cp1.gdb
set pagination off
set print thread-events off

echo \n==== info threads ====\n
info threads

# -------- chiplet 0 CP1 (threads 5-8) --------
echo \n==== chiplet 0 CP1.cpu0 (thread 5) ====\n
thread 5
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 0 CP1.cpu1 (thread 6) ====\n
thread 6
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 0 CP1.cpu2 (thread 7) ====\n
thread 7
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 0 CP1.cpu3 (thread 8) ====\n
thread 8
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

# -------- chiplet 1 CP1 (threads 13-16) --------
echo \n==== chiplet 1 CP1.cpu0 (thread 13) ====\n
thread 13
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 1 CP1.cpu1 (thread 14) ====\n
thread 14
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 1 CP1.cpu2 (thread 15) ====\n
thread 15
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 1 CP1.cpu3 (thread 16) ====\n
thread 16
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

# -------- chiplet 2 CP1 (threads 21-24) --------
echo \n==== chiplet 2 CP1.cpu0 (thread 21) ====\n
thread 21
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 2 CP1.cpu1 (thread 22) ====\n
thread 22
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 2 CP1.cpu2 (thread 23) ====\n
thread 23
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 2 CP1.cpu3 (thread 24) ====\n
thread 24
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

# -------- chiplet 3 CP1 (threads 29-32) --------
echo \n==== chiplet 3 CP1.cpu0 (thread 29) ====\n
thread 29
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 3 CP1.cpu1 (thread 30) ====\n
thread 30
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 3 CP1.cpu2 (thread 31) ====\n
thread 31
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== chiplet 3 CP1.cpu3 (thread 32) ====\n
thread 32
frame 0
x/1i $pc
printf "ELR_EL3 = 0x%llx  ESR_EL3 = 0x%llx  FAR_EL3 = 0x%llx\n", $ELR_EL3, $ESR_EL3, $FAR_EL3

echo \n==== inspection done ====\n
