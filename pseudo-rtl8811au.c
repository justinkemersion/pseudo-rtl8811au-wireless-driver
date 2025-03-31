#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/firmware.h>

// Table of supported USB devices
static struct usb_device_id my_table[] = {
    {USB_DEVICE(0x2357, 0x011e)}, // RTL8811AU
    {} /* Terminating entry */
};

// Driver structure with driver information and callback functions
static struct usb_driver my_driver = {
    .name = "rtl8811au",
    .id_table = my_table,
    .probe = my_probe,
    .disconnect = my_disconnect,
};

// Probe function called when a supported USB device is detected
static int my_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    // Get the USB device associated with the interface
    struct usb_device *dev = usb_get_dev(intf->usb_dev);

    // Load firmware onto the device if necessary
    int ret = load_firmware(dev);
    if (ret) {
        printk(KERN_ERR "Failed to load firmware\n");
        return ret;
    }

    // Initialize the device and register it with the USB subsystem
    ret = init_device(dev);
    if (ret) {
        printk(KERN_ERR "Failed to initialize device\n");
        return ret;
    }

    // Print a success message to dmesg
    printk(KERN_INFO "RTL8811AU successfully initialized with firmware\n");

    return 0;
}

// Load firmware onto the device
static int load_firmware(struct usb_device *dev) {
    struct firmware *fw = NULL;
    int ret;

    // Load the firmware file
    ret = request_firmware(&fw, "rtl8811au.fw", dev);
    if (ret < 0) {
        printk(KERN_ERR "Failed to load firmware file\n");
        return ret;
    }

    // Load the firmware onto the device
    ret = usb_load_firmware(dev, fw);
    if (ret < 0) {
        printk(KERN_ERR "Failed to load firmware\n");
        return ret;
    }

    // Free the firmware file
    release_firmware(fw);

    return 0;
}

// Initialize the device and register it with the USB subsystem
static int init_device(struct usb_device *dev) {
    struct usb_driver *drv = NULL;
    int ret;

    // Set up the driver for the device
    drv = usb_alloc_driver(dev, &my_driver);
    if (!drv) {
        printk(KERN_ERR "Failed to allocate driver\n");
        return -ENOMEM;
    }

    // Register the driver with the USB subsystem
    ret = usb_register_driver(drv, dev);
    if (ret < 0) {
        printk(KERN_ERR "Failed to register driver\n");
        return ret;
    }

    return 0;
}