#!/bin/bash

# Script for managing ArchPrebuilt VM and rtl8811au_wifi driver project
# Last updated: March 30, 2025

# Variables
VM_NAME="ArchPrebuilt"
SOURCE_DIR="/home/user/rtl8811au"  # Adjust to your local source directory
VM_USER="osboxes"
VM_HOST="localhost"
VM_PORT="2222"
VM_DEST_DIR="/home/osboxes"
SERIAL_LOG="/tmp/archprebuilt-serial.log"

echo "=== RTL8811AU WiFi Driver Project Commands ==="

# --- VirtualBox Management ---
echo -e "\n# Start the VM in headless mode"
echo "VBoxHeadless --startvm \"$VM_NAME\" &"

echo -e "\n# Stop the VM"
echo "VBoxManage controlvm \"$VM_NAME\" poweroff"

echo -e "\n# List running VMs"
echo "VBoxManage list runningvms"

echo -e "\n# Check VM detailed info (e.g., UART settings)"
echo "VBoxManage showvminfo \"$VM_NAME\" | grep UART"

echo -e "\n# Force reset VM (if stuck)"
echo "VBoxManage controlvm \"$VM_NAME\" reset"

echo -e "\n# Discard saved state (if locked)"
echo "VBoxManage discardstate \"$VM_NAME\""

echo -e "\n# Set up serial logging (run when VM is off)"
echo "VBoxManage modifyvm \"$VM_NAME\" --uart1 0x3F8 4 --uartmode1 file $SERIAL_LOG"

# --- File Transfers with SCP ---
echo -e "\n# SCP a single file (e.g., compiled driver) to VM"
echo "scp -P $VM_PORT $SOURCE_DIR/rtl8811au_wifi.ko $VM_USER@$VM_HOST:$VM_DEST_DIR/"

echo -e "\n# SCP an entire directory (e.g., source code) to VM"
echo "scp -P $VM_PORT -r $SOURCE_DIR $VM_USER@$VM_HOST:$VM_DEST_DIR/"

# --- SSH into VM ---
echo -e "\n# SSH into the VM"
echo "ssh -p $VM_PORT $VM_USER@$VM_HOST"

# --- Driver Management Inside VM ---
echo -e "\n# Load the driver (run inside VM)"
echo "sudo insmod $VM_DEST_DIR/rtl8811au_wifi.ko"

echo -e "\n# Unload the driver (run inside VM)"
echo "sudo rmmod rtl8811au_wifi"

echo -e "\n# Check network interfaces (run inside VM)"
echo "ip addr"

echo -e "\n# Bring wlan0 up (run inside VM)"
echo "sudo ip link set wlan0 up"

echo -e "\n# Bring wlan0 down (run inside VM)"
echo "sudo ip link set wlan0 down"

echo -e "\n# Check network interface status (run inside VM)"
echo "ip link"

echo -e "\n# Scan with wlan0 (run inside VM)"
echo "sudo iw dev wlan0 scan"

echo -e "\n# View kernel logs (run inside VM)"
echo "dmesg | tail -n 50"

# --- Serial Log Viewing on Host ---
echo -e "\n# View last 50 lines of serial log (on host)"
echo "cat $SERIAL_LOG | tail -n 50"

echo -e "\n# Live monitor serial log (on host)"
echo "tail -f $SERIAL_LOG"

# --- Compilation on Host ---
echo -e "\n# Compile the driver (on host)"
echo "cd $SOURCE_DIR"
echo "make clean"
echo "make"

# --- Miscellaneous ---
echo -e "\n# Check USB devices (on host or VM)"
echo "lsusb"

echo -e "\n# View USB device details (on host or VM)"
echo "lsusb -v -d 2357:011e"

echo -e "\n# Check loaded kernel modules (on host or VM)"
echo "lsmod | grep -E \"wifi|80211|rtl|rtw\""

echo -e "\n# Blacklist competing drivers (on VM, edit /etc/modprobe.d/blacklist-rtl.conf)"
echo "echo \"blacklist rtw_8821au\" | sudo tee /etc/modprobe.d/blacklist-rtl.conf"
echo "echo \"blacklist rtw88_usb\" | sudo tee -a /etc/modprobe.d/blacklist-rtl.conf"
echo "sudo mkinitcpio -P"
echo "sudo reboot"

echo -e "\n# Reboot VM (inside VM)"
echo "sudo reboot"

echo -e "\n# Install wireless tools (inside VM)"
echo "sudo pacman -S iw wireless_tools wpa_supplicant dhcpcd"

echo -e "\n# Check driver binding for wlan0 (inside VM)"
echo "readlink /sys/class/net/wlan0/device/driver"

echo -e "\n=== End of Commands ==="
echo "Adjust paths (SOURCE_DIR, VM_DEST_DIR) as needed for your setup!"