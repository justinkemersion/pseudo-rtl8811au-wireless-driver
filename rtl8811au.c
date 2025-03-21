#include <linux/module.h>
#include <linux/usb.h>
#include <linux/firmware.h>
#include <linux/delay.h>

#define VENDOR_ID  0x2357
#define PRODUCT_ID 0x011e

static struct usb_device_id my_table[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    { }
};
MODULE_DEVICE_TABLE(usb, my_table);

static int write_reg(struct usb_device *dev, u16 reg, u8 value) {
    int ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                              0x05, USB_DIR_OUT | USB_TYPE_VENDOR,
                              value, reg, NULL, 0, 2000);
    printk(KERN_INFO "RTL8811AU: write_reg 0x%04x = 0x%02x, ret: %d\n", reg, value, ret);
    return ret;
}

static int read_reg(struct usb_device *dev, u16 reg) {
    u8 value = 0;
    int ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
                              0x05, USB_DIR_IN | USB_TYPE_VENDOR,
                              0, reg, &value, 1, 2000);
    printk(KERN_INFO "RTL8811AU: read_reg 0x%04x = 0x%02x, ret: %d\n", reg, value, ret);
    return ret < 0 ? ret : value;
}

static int init_device(struct usb_device *dev) {
    int ret, i;

    printk(KERN_INFO "RTL8811AU: Starting init_device - timing probe\n");

    // Step 1: USB reset
    printk(KERN_INFO "RTL8811AU: Attempting usb_reset_device\n");
    ret = usb_reset_device(dev);
    if (ret < 0) {
        printk(KERN_ERR "RTL8811AU: usb_reset_device failed: %d\n", ret);
        return ret;
    }
    printk(KERN_INFO "RTL8811AU: usb_reset_device succeeded\n");

    // Step 2: Long delay and read retry
    for (i = 0; i < 10; i++) {
        msleep(500); // 500ms increments, up to 5s
        printk(KERN_INFO "RTL8811AU: Read attempt %d after %dms\n", i + 1, (i + 1) * 500);
        ret = read_reg(dev, 0x00);
        if (ret >= 0) {
            printk(KERN_INFO "RTL8811AU: Read succeeded: 0x%02x\n", ret);
            goto init;
        }
        printk(KERN_ERR "RTL8811AU: Read failed: %d\n", ret);
    }

    // Step 3: Wake attempt
    printk(KERN_INFO "RTL8811AU: Wake attempt: bRequest=0x05, wValue=0x01, wIndex=0x0002\n");
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x05, USB_DIR_OUT | USB_TYPE_VENDOR,
                          0x01, 0x0002, NULL, 0, 5000); // Longer timeout
    if (ret < 0) {
        printk(KERN_ERR "RTL8811AU: Wake failed: %d\n", ret);
    } else {
        printk(KERN_INFO "RTL8811AU: Wake succeeded\n");
        msleep(20);
        ret = read_reg(dev, 0x00);
        if (ret >= 0) {
            printk(KERN_INFO "RTL8811AU: Read succeeded post-wake: 0x%02x\n", ret);
            goto init;
        }
        printk(KERN_ERR "RTL8811AU: Read failed post-wake: %d\n", ret);
    }

    printk(KERN_ERR "RTL8811AU: All attempts failed\n");
    return -ETIMEDOUT;

init:
    // Step 4: Clear firmware state
    ret = write_reg(dev, 0x80, 0x00);
    if (ret < 0) return ret;
    msleep(10);

    // Step 5: Enable MCU
    ret = write_reg(dev, 0x03, 0x51);
    if (ret < 0) return ret;
    msleep(20);

    // Step 6: Check readiness
    ret = read_reg(dev, 0x80);
    if (ret < 0) return ret;
    if (!(ret & 0x01)) {
        printk(KERN_ERR "RTL8811AU: MCU not ready\n");
        return -EIO;
    }
    printk(KERN_INFO "RTL8811AU: Init complete\n");
    return 0;
}

static int load_firmware(struct usb_device *dev) {
    const struct firmware *fw;
    int ret;

    ret = request_firmware(&fw, "rtlwifi/rtl8811aufw.bin", &dev->dev);
    if (ret) {
        printk(KERN_ERR "RTL8811AU: Firmware rtl8811aufw.bin not found\n");
        return ret;
    }
    printk(KERN_INFO "RTL8811AU: Firmware size: %zu\n", fw->size);

    ret = write_reg(dev, 0x80, 0x01);
    if (ret < 0) {
        printk(KERN_ERR "RTL8811AU: Enable firmware failed: %d\n", ret);
        goto out;
    }
    msleep(50);

    ret = usb_bulk_msg(dev, usb_sndbulkpipe(dev, 0x05), (void *)fw->data, fw->size, NULL, 5000);
    if (ret) {
        printk(KERN_ERR "RTL8811AU: usb_bulk_msg failed: %d\n", ret);
        goto out;
    }

    ret = read_reg(dev, 0x80);
    if (ret < 0 || !(ret & 0x02)) {
        printk(KERN_ERR "RTL8811AU: Firmware not confirmed\n");
        ret = -EIO;
    } else {
        printk(KERN_INFO "RTL8811AU: Firmware loaded successfully\n");
        ret = 0;
    }

out:
    release_firmware(fw);
    return ret;
}

static int my_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    struct usb_device *dev = interface_to_usbdev(intf);
    int ret;

    printk(KERN_INFO "RTL8811AU: Detected %04x:%04x\n", id->idVendor, id->idProduct);

    ret = init_device(dev);
    if (ret) {
        printk(KERN_ERR "RTL8811AU: Init failed: %d\n", ret);
        return ret;
    }

    printk(KERN_INFO "RTL8811AU: Init succeeded, starting firmware load\n");

    ret = load_firmware(dev);
    if (ret) {
        printk(KERN_ERR "RTL8811AU: Firmware load failed: %d\n", ret);
        return ret;
    }

    printk(KERN_INFO "RTL8811AU: Device initialized\n");
    return 0;
}

static void my_disconnect(struct usb_interface *intf) {
    printk(KERN_INFO "RTL8811AU: Disconnected\n");
}

static struct usb_driver my_driver = {
    .name = "rtl8811au_minimal",
    .id_table = my_table,
    .probe = my_probe,
    .disconnect = my_disconnect,
};

static int __init my_init(void) {
    printk(KERN_INFO "RTL8811AU Minimal Driver Loading\n");
    return usb_register(&my_driver);
}

static void __exit my_exit(void) {
    printk(KERN_INFO "RTL8811AU Minimal Driver Unloading\n");
    usb_deregister(&my_driver);
}

module_init(my_init);
module_exit(my_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal RTL8811AU driver for 2357:011e");