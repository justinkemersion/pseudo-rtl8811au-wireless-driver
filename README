### `README.md`

```markdown
# RTL8811AU WiFi Driver Project

A custom Linux kernel module (`rtl8811au_wifi.ko`) for the TP-Link AC600 USB WiFi adapter (Vendor ID: `0x2357`, Product ID: `0x011e`). This project aims to create a functional driver with basic WiFi capabilities (scanning, potentially connecting) using USB bulk endpoints and `cfg80211`.

**Current Status**: In progress. The driver loads, binds to the device, and creates `wlan0`, but `ip link set wlan0 up` fails with "RTNETLINK answers: No such device" due to `priv->usb_dev` being null in `rtl8811au_open()`. Debugging is ongoing.

**Last Updated**: March 31, 2025

## Project Overview
- **Goal**: Develop a minimal WiFi driver for learning and experimentation.
- **Environment**: Tested on Arch Linux (host with `linux-hardened`) and an "ArchPrebuilt" VM via VirtualBox.
- **Features Implemented**:
  - USB device detection and binding.
  - Firmware loading (`rtl8811au/rtl8811au_fw.bin`).
  - `wiphy` and `netdev` registration, creating `wlan0`.
  - Debug logging for troubleshooting.
- **Issues**:
  - `priv->usb_dev` is set in `probe()` but null in `open()`, causing RX URB setup to fail.
  - Multiple `wlan0` instances (e.g., `wlan0@4`, `wlan0@5`) on reload, suggesting cleanup issues.

## Prerequisites
- **Host**: Arch Linux with `linux-headers`, `base-devel`, `git`.
- **VM**: VirtualBox with "ArchPrebuilt" VM (OSBoxes Arch image).
- **Tools**: `make`, `gcc`, `scp`, `ssh`, `iw`, `wireless_tools`, `wpa_supplicant`, `dhcpcd`.
- **Firmware**: `rtl8811au/rtl8811au_fw.bin` in `/lib/firmware/rtl8811au/` (copy from host if needed).

## Setup
1. **Clone or Copy Source**:
   ```bash
   git clone <your-repo>  # If hosted
   cd rtl8811au
   ```
   - Or copy `rtl8811au.c` to a working directory (e.g., `/home/user/rtl8811au`).

2. **Install Dependencies (Host)**:
   ```bash
   sudo pacman -S base-devel linux-headers git virtualbox
   ```

3. **Set Up VM**:
   - Import "ArchPrebuilt" VM in VirtualBox.
   - Configure NAT port forwarding:
     ```bash
     VBoxManage modifyvm "ArchPrebuilt" --natpf1 "ssh,tcp,,2222,,22"
     ```
   - Set up serial logging:
     ```bash
     VBoxManage modifyvm "ArchPrebuilt" --uart1 0x3F8 4 --uartmode1 file /tmp/archprebuilt-serial.log
     ```

4. **Install VM Tools**:
   ```bash
   ssh -p 2222 osboxes@localhost
   sudo pacman -Syu
   sudo pacman -S iw wireless_tools wpa_supplicant dhcpcd
   ```

5. **Transfer Source to VM**:
   ```bash
   scp -P 2222 -r /home/user/rtl8811au osboxes@localhost:/home/osboxes/
   ```

## Usage
1. **Compile on Host**:
   ```bash
   cd /home/user/rtl8811au
   make clean
   make
   ```

2. **Start VM**:
   ```bash
   VBoxHeadless --startvm "ArchPrebuilt" &
   ```

3. **SSH and Load Driver**:
   ```bash
   ssh -p 2222 osboxes@localhost
   sudo insmod /home/osboxes/rtl8811au/rtl8811au_wifi.ko
   ip addr  # Check wlan0
   ```

4. **Test Interface**:
   ```bash
   sudo ip link set wlan0 up  # Currently fails
   ip link
   sudo iw dev wlan0 scan
   ```

5. **View Logs**:
   - Inside VM:
     ```bash
     dmesg | tail -n 50
     ```
   - On host (serial log):
     ```bash
     cat /tmp/archprebuilt-serial.log | tail -n 50
     ```

## Debugging
- **Current Issue**: `priv->usb_dev` is null in `rtl8811au_open()`, despite being set in `probe()`.
  - `probe` priv: e.g., `00000000d0dd30b4`
  - `open` priv: e.g., `00000000e358f978`
  - Check: Compare "Priv at" pointers in logs.

- **Steps**:
  1. Load driver:
     ```bash
     sudo insmod /home/osboxes/rtl8811au/rtl8811au_wifi.ko
     ```
  2. Check `wlan0`:
     ```bash
     ip addr
     ```
  3. Attempt to bring up:
     ```bash
     sudo ip link set wlan0 up
     ```
  4. View logs:
     ```bash
     cat /tmp/archprebuilt-serial.log | tail -n 50
     ```

- **Useful Commands**: See `rtl8811au_project.sh` for a full list.

## Todos
1. **Fix `priv->usb_dev` Null**:
   - Ensure `netdev_priv()` returns the same `priv` from `probe()` to `open()`.
   - Debug `alloc_netdev()` and `usb_set_intfdata()` linkage.

2. **Resolve Multiple `wlan0` Instances**:
   - Enhance `rtl8811au_disconnect()` to fully unregister and free `net_dev`.
   - Test unload/reload cycle:
     ```bash
     sudo rmmod rtl8811au_wifi
     ip addr
     sudo insmod /home/osboxes/rtl8811au/rtl8811au_wifi.ko
     ```

3. **Enable RX URB Setup**:
   - Once `usb_dev` is fixed, complete RX URB initialization in `open()`.

4. **Test Scanning**:
   - Verify `iw dev wlan0 scan` works after `wlan0` is up.

## Script Reference
See `rtl8811au_project.sh` for all commands:
- VM start/stop/list
- File transfers (`scp`)
- Driver load/unload
- Network interface management (`ip link`, `iw`)
- Log viewing (`dmesg`, serial)

To use:
```bash
chmod +x rtl8811au_project.sh
./rtl8811au_project.sh
```

## Notes
- **File**: `rtl8811au.c` (latest version with debug prints).
- **Firmware**: Ensure `/lib/firmware/rtl8811au/rtl8811au_fw.bin` exists in the VM.
- **Blacklist**: Competing drivers (`rtw_8821au`, `rtw88_usb`) are blacklisted in `/etc/modprobe.d/blacklist-rtl.conf`.

## Getting Back In
1. Review this README.
2. Run `rtl8811au_project.sh` to see commands.
3. Start VM, compile, and test:
   ```bash
   VBoxHeadless --startvm "ArchPrebuilt" &
   cd /home/user/rtl8811au
   make clean && make
   scp -P 2222 ./rtl8811au_wifi.ko osboxes@localhost:/home/osboxes/
   ssh -p 2222 osboxes@localhost
   sudo insmod /home/osboxes/rtl8811au_wifi.ko
   ```