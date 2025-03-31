#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/firmware.h>
#include <linux/netdevice.h>
#include <linux/wireless.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
// #include <linux/completion.h> // Removed, tx_complete was unused
#include <linux/delay.h> // Keep for mdelay/udelay if needed later, but avoid msleep in atomic
#include <linux/workqueue.h>

// Device Vendor and Product IDs
#define USB_VENDOR_ID_TP_LINK 0x2357
#define USB_PRODUCT_ID_AC600_NANO 0x011e
#define RTL8811AU_FIRMWARE "rtl8811au/rtl8811au_fw.bin"

// Maximum packet size
#define MAX_PACKET_SIZE 2048
#define MAX_RX_ERRORS 5 // Example: Allow up to 5 consecutive errors before stopping RX resubmits

// Driver structure
struct rtl8811au_dev {
    struct usb_device *usb_dev;
    struct usb_interface *usb_intf;
    const struct firmware *firmware;
    struct wiphy *wiphy;
    struct net_device *net_dev;

    // USB URB management
    struct urb *rx_urb;
    int rx_error_count; // Track RX errors
    unsigned char *rx_buffer;
    dma_addr_t rx_dma;
    struct workqueue_struct *tx_wq;         // TX Workqueue
    struct sk_buff_head tx_queue;           // Queue for outgoing packets
    struct work_struct tx_worker_work;      // Work struct for TX worker
    spinlock_t tx_queue_lock;               // Lock for tx_queue
    atomic_t tx_busy;                       // Flag: 1 if a TX URB is currently in flight
    struct sk_buff *tx_skb;                 // Pointer to the SKB currently being transmitted (FIXED)

    // Spinlocks
    // spinlock_t tx_lock; // Removed, unused
    spinlock_t stats_lock;                  // Lock for net_device stats

    // Tx completion
    // struct completion tx_complete; // Removed, unused

    // Dynamically discovered endpoints
    unsigned char bulk_in_endpoint;
    unsigned char bulk_out_endpoint;
};

// USB Device ID table
static struct usb_device_id rtl8811au_table[] = {
    { USB_DEVICE(USB_VENDOR_ID_TP_LINK, USB_PRODUCT_ID_AC600_NANO) },
    { } /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, rtl8811au_table);

// --- Forward Declarations ---
static int rtl8811au_open(struct net_device *dev);
static int rtl8811au_stop(struct net_device *dev);
static netdev_tx_t rtl8811au_xmit(struct sk_buff *skb, struct net_device *dev);
static void rtl8811au_tx_worker(struct work_struct *work);
static void rtl8811au_tx_complete(struct urb *urb);
static void rtl8811au_rx_complete(struct urb *urb);
static int rtl8811au_set_mac_address(struct net_device *dev, void *addr);

// --- cfg80211 Operations ---
// NOTE: This is a placeholder. Real scan functionality is needed.
static int rtl8811au_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request) {
    struct rtl8811au_dev *priv = wiphy_priv(wiphy); // Get priv pointer
    printk(KERN_INFO "%s: Scan requested (dummy)\n", priv->net_dev->name);
    // TODO: Implement actual hardware scan triggering
    // For now, just report scan finished (aborted) immediately
    //cfg80211_scan_done(request, true); // Use 'true' for aborted/failed scan
    return 0; // Return 0 for success in initiating scan (even if dummy)
}

// NOTE: Add other necessary cfg80211 ops (connect, disconnect, set_channel, etc.)
static struct cfg80211_ops rtl8811au_cfg80211_ops = {
    .scan = rtl8811au_scan,
    // .connect = rtl8811au_connect, // Example future op
    // .disconnect = rtl8811au_disconnect_station, // Example future op
    // .set_wiphy_params = rtl8811au_set_wiphy_params, // Example future op
};

// --- Netdevice Operations ---
static const struct net_device_ops rtl8811au_netdev_ops = {
    .ndo_open = rtl8811au_open,
    .ndo_stop = rtl8811au_stop,
    .ndo_start_xmit = rtl8811au_xmit,
    .ndo_set_mac_address = rtl8811au_set_mac_address,
    // .ndo_get_stats64 = ..., // Consider implementing for detailed stats
};

// Wireless device structure associated with net_device
static struct wireless_dev rtl8811au_wdev = {
    .iftype = NL80211_IFTYPE_STATION,
    // wiphy will be set during probe
};

// --- Open Function ---
static int rtl8811au_open(struct net_device *dev) {
    struct rtl8811au_dev *priv = netdev_priv(dev);
    int ret;

    printk(KERN_INFO "%s: Opening network device\n", dev->name);

    // Basic sanity checks
    if (!priv) {
        printk(KERN_ERR "rtl8811au_wifi: Private data is NULL in open\n");
        return -EINVAL;
    }
    if (!priv->usb_dev) {
        printk(KERN_ERR "%s: USB device is NULL in open\n", dev->name);
        return -ENODEV;
    }
    if (priv->bulk_in_endpoint == 0) {
        printk(KERN_ERR "%s: Bulk IN endpoint not found\n", dev->name);
        return -ENODEV;
    }

    // Allocate RX URB
    priv->rx_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!priv->rx_urb) {
        printk(KERN_ERR "%s: Failed to allocate RX URB\n", dev->name);
        return -ENOMEM;
    }

    // Allocate RX buffer (DMA coherent)
    priv->rx_buffer = usb_alloc_coherent(priv->usb_dev, MAX_PACKET_SIZE, GFP_KERNEL, &priv->rx_dma);
    if (!priv->rx_buffer) {
        printk(KERN_ERR "%s: Failed to allocate RX buffer\n", dev->name);
        usb_free_urb(priv->rx_urb);
        priv->rx_urb = NULL;
        return -ENOMEM;
    }

    // Fill the RX URB
    usb_fill_bulk_urb(priv->rx_urb, priv->usb_dev,
                      usb_rcvbulkpipe(priv->usb_dev, priv->bulk_in_endpoint),
                      priv->rx_buffer, MAX_PACKET_SIZE,
                      rtl8811au_rx_complete, priv);
    priv->rx_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP; // Use pre-allocated coherent buffer
    priv->rx_urb->transfer_dma = priv->rx_dma;


    // Submit the initial RX URB
    priv->rx_error_count = 0; // Reset error count on open
    //ret = usb_submit_urb(priv->rx_urb, GFP_KERNEL);
    /*if (ret) {
        printk(KERN_ERR "%s: Failed to submit initial RX URB (error %d)\n", dev->name, ret);
        usb_free_coherent(priv->usb_dev, MAX_PACKET_SIZE, priv->rx_buffer, priv->rx_dma);
        usb_free_urb(priv->rx_urb);
        priv->rx_urb = NULL;
        priv->rx_buffer = NULL;
        return ret;
    }*/

    // Start the network queue (allows xmit function to be called)
    netif_start_queue(dev);
    printk(KERN_INFO "%s: Network queue started\n", dev->name);
    return 0;
}

// --- Stop Function ---
static int rtl8811au_stop(struct net_device *dev) {
    struct rtl8811au_dev *priv = netdev_priv(dev);
    printk(KERN_INFO "%s: Stopping network device\n", dev->name);

    // Stop the network queue (prevents new transmissions)
    netif_stop_queue(dev);

    // Kill the pending RX URB
    // Needs to be done before freeing buffer
    if (priv->rx_urb) {
        usb_kill_urb(priv->rx_urb); // Wait until URB is not running
    }

    // --- Workqueue cleanup moved to disconnect ---
    // cancel_work_sync(&priv->tx_worker_work); // Ensure TX worker isn't running
    // flush_workqueue(priv->tx_wq); // Ensure queue is empty
    // destroy_workqueue(priv->tx_wq); // FIXED: Moved to disconnect
    // priv->tx_wq = NULL;

    // Free RX resources
    if (priv->rx_urb) {
        usb_free_urb(priv->rx_urb);
        priv->rx_urb = NULL;
    }
    if (priv->rx_buffer) {
        usb_free_coherent(priv->usb_dev, MAX_PACKET_SIZE, priv->rx_buffer, priv->rx_dma);
        priv->rx_buffer = NULL;
    }

    // TODO: Add hardware de-initialization commands if necessary

    printk(KERN_INFO "%s: Network device stopped\n", dev->name);
    return 0;
}

// --- Transmit Function (called by kernel) ---
static netdev_tx_t rtl8811au_xmit(struct sk_buff *skb, struct net_device *dev) {
    struct rtl8811au_dev *priv = netdev_priv(dev);
    unsigned long flags;

    // Don't transmit if device is not running or being removed
    if (!netif_running(dev) || !priv || !priv->tx_wq) {
        dev_kfree_skb_any(skb); // Free the skb
        dev->stats.tx_dropped++;
        return NETDEV_TX_OK;
    }

    // Check if TX endpoint exists
    if (priv->bulk_out_endpoint == 0) {
         printk_once(KERN_ERR "%s: No bulk OUT endpoint for TX!\n", dev->name);
         dev_kfree_skb_any(skb);
         dev->stats.tx_dropped++;
         return NETDEV_TX_OK;
    }

    // Queue the packet
    spin_lock_irqsave(&priv->tx_queue_lock, flags);
    // Basic backpressure: Stop queue if it gets too long
    if (skb_queue_len(&priv->tx_queue) > 100) { // Example queue limit
        netif_stop_queue(dev);
        // We still queue the packet but signal TX busy to the kernel
        skb_queue_tail(&priv->tx_queue, skb);
        spin_unlock_irqrestore(&priv->tx_queue_lock, flags);
        printk(KERN_DEBUG "%s: TX queue full, stopping queue\n", dev->name);
        return NETDEV_TX_BUSY; // Indicate busy, kernel will retry later
    }

    // Add packet to the queue
    skb_queue_tail(&priv->tx_queue, skb);
    spin_unlock_irqrestore(&priv->tx_queue_lock, flags);

    // Schedule the worker if it's not already busy processing a previous URB
    if (atomic_read(&priv->tx_busy) == 0) {
        queue_work(priv->tx_wq, &priv->tx_worker_work);
    }

    return NETDEV_TX_OK; // Packet accepted
}

// --- TX Worker Function (runs in process context from workqueue) ---
static void rtl8811au_tx_worker(struct work_struct *work) {
    struct rtl8811au_dev *priv = container_of(work, struct rtl8811au_dev, tx_worker_work);
    struct sk_buff *skb;
    unsigned long flags;
    int ret;
    unsigned char *tx_buffer; // DMA buffer for TX
    dma_addr_t tx_dma;        // DMA address for TX buffer
    struct urb *tx_urb;       // URB for TX
    unsigned int len;
    struct net_device_stats *stats = &priv->net_dev->stats;

    // Loop while there are packets and we are not already busy with a URB
    while (true) {
        // Try to grab a packet from the queue
        spin_lock_irqsave(&priv->tx_queue_lock, flags);
        skb = skb_dequeue(&priv->tx_queue);
        if (!skb) {
            // No more packets, exit the loop
            spin_unlock_irqrestore(&priv->tx_queue_lock, flags);
            break;
        }

        // Try to mark TX as busy. If already busy, requeue packet and exit.
        // This prevents submitting multiple TX URBs simultaneously in this simple model.
        if (atomic_xchg(&priv->tx_busy, 1) != 0) {
            // Already busy, put packet back at the head and try later
            skb_queue_head(&priv->tx_queue, skb);
            spin_unlock_irqrestore(&priv->tx_queue_lock, flags);
            // No need to queue_work again, the completion will handle it
            printk(KERN_DEBUG "%s: TX worker - busy, delaying packet\n", priv->net_dev->name);
            break; // Exit the loop, wait for current URB to complete
        }
        spin_unlock_irqrestore(&priv->tx_queue_lock, flags);

        // --- We are now marked busy and have a packet (skb) ---

        len = skb->len;
        // Sanity check packet length (should ideally be handled by higher layers)
        if (len > MAX_PACKET_SIZE) {
            dev_err(&priv->usb_intf->dev, "%s: Oversized packet (%d > %d)\n", priv->net_dev->name, len, MAX_PACKET_SIZE);
            spin_lock_irqsave(&priv->stats_lock, flags);
            stats->tx_dropped++;
            spin_unlock_irqrestore(&priv->stats_lock, flags);
            dev_kfree_skb_any(skb); // Free the oversized skb
            atomic_set(&priv->tx_busy, 0); // Clear busy flag
            continue; // Try next packet
        }

        // Allocate TX URB
        tx_urb = usb_alloc_urb(0, GFP_KERNEL);
        if (!tx_urb) {
            dev_err(&priv->usb_intf->dev, "%s: Failed to allocate TX URB\n", priv->net_dev->name);
            spin_lock_irqsave(&priv->stats_lock, flags);
            stats->tx_dropped++;
            spin_unlock_irqrestore(&priv->stats_lock, flags);
            // Put packet back at head for retry later? Or just drop? Dropping is simpler.
            dev_kfree_skb_any(skb);
            atomic_set(&priv->tx_busy, 0); // Clear busy flag
            continue; // Try next packet
        }

        // Allocate DMA buffer for this TX operation
        // Note: Allocating per packet might be inefficient for high rates.
        tx_buffer = usb_alloc_coherent(priv->usb_dev, len, GFP_KERNEL, &tx_dma);
        if (!tx_buffer) {
            dev_err(&priv->usb_intf->dev, "%s: Failed to allocate TX buffer (len %d)\n", priv->net_dev->name, len);
            spin_lock_irqsave(&priv->stats_lock, flags);
            stats->tx_dropped++;
            spin_unlock_irqrestore(&priv->stats_lock, flags);
            dev_kfree_skb_any(skb);
            usb_free_urb(tx_urb);
            atomic_set(&priv->tx_busy, 0); // Clear busy flag
            continue; // Try next packet
        }

        // Copy packet data to DMA buffer
        memcpy(tx_buffer, skb->data, len);

        // Store skb pointer in private struct (FIXED: context handling)
        priv->tx_skb = skb;

        // Fill the TX URB
        usb_fill_bulk_urb(tx_urb, priv->usb_dev,
                          usb_sndbulkpipe(priv->usb_dev, priv->bulk_out_endpoint),
                          tx_buffer, len,
                          rtl8811au_tx_complete,
                          priv); // Pass priv structure as context (FIXED)
        tx_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP; // Use pre-allocated coherent buffer
        tx_urb->transfer_dma = tx_dma;

        // Submit the TX URB
        ret = usb_submit_urb(tx_urb, GFP_KERNEL);
        if (ret) {
            dev_err(&priv->usb_intf->dev, "%s: Failed to submit TX URB (error %d)\n", priv->net_dev->name, ret);
            spin_lock_irqsave(&priv->stats_lock, flags);
            stats->tx_errors++;
            stats->tx_dropped++; // Also count as dropped if submit fails
            spin_unlock_irqrestore(&priv->stats_lock, flags);

            usb_free_coherent(priv->usb_dev, len, tx_buffer, tx_dma); // Free buffer on error
            priv->tx_skb = NULL; // Clear skb pointer
            dev_kfree_skb_any(skb); // Free skb
            usb_free_urb(tx_urb); // Free URB
            atomic_set(&priv->tx_busy, 0); // Clear busy flag
            continue; // Try next packet
        }

        // Successfully submitted URB, update stats
        spin_lock_irqsave(&priv->stats_lock, flags);
        stats->tx_packets++;
        stats->tx_bytes += len;
        spin_unlock_irqrestore(&priv->stats_lock, flags);

        // URB is in flight, the worker must wait for completion before sending next packet.
        // Break out of the loop. The completion handler will clear tx_busy
        // and potentially re-queue the worker if needed.
        break;

    } // end while(true)

    // If we broke out of the loop because the queue was empty,
    // ensure the network queue is awake (if it was stopped)
    spin_lock_irqsave(&priv->tx_queue_lock, flags);
    if (skb_queue_len(&priv->tx_queue) < 50) { // Threshold to wake queue
       if (netif_queue_stopped(priv->net_dev) && atomic_read(&priv->tx_busy) == 0) {
           netif_wake_queue(priv->net_dev);
           printk(KERN_DEBUG "%s: TX queue woken up by worker\n", priv->net_dev->name);
       }
    }
    spin_unlock_irqrestore(&priv->tx_queue_lock, flags);
}


// --- TX Completion Handler (runs in atomic context) ---
static void rtl8811au_tx_complete(struct urb *urb) {
    // Get private data structure from URB context (FIXED)
    struct rtl8811au_dev *priv = urb->context;
    struct sk_buff *skb; // SKB pointer will be retrieved from priv
    struct net_device_stats *stats;
    unsigned long flags;
    int status = urb->status;
    bool queue_was_stopped;
    bool work_queued = false;

    // Basic sanity checks
    if (!priv || !priv->net_dev) {
        printk(KERN_ERR "rtl8811au_wifi: Invalid context or net_dev in TX complete\n");
        // Can't do much else here, resources might leak if buffer/urb aren't freed
        if (urb->transfer_buffer) {
             // Guess size is urb->transfer_buffer_length? Risky.
             // usb_free_coherent might need device, size, cpu_addr, dma_addr
        }
        usb_free_urb(urb);
        return;
    }

    stats = &priv->net_dev->stats;
    skb = priv->tx_skb; // Retrieve SKB pointer (FIXED)

    if (!skb) {
       printk(KERN_ERR "%s: TX complete but skb pointer was NULL!\n", priv->net_dev->name);
       // Don't try to free skb, but do free USB resources
    }

    // Check URB status
    if (status != 0) {
        printk(KERN_ERR "%s: TX URB failed (status %d)\n", priv->net_dev->name, status);
        spin_lock_irqsave(&priv->stats_lock, flags);
        stats->tx_errors++;
        // Note: tx_dropped was already counted if submit failed.
        // If it fails here, it means submit succeeded but transfer failed.
        spin_unlock_irqrestore(&priv->stats_lock, flags);
        // Status codes like -EPIPE, -ENODEV indicate device issues
    }

    // Free the DMA buffer associated with this URB
    usb_free_coherent(priv->usb_dev,
                      urb->transfer_buffer_length, // Size used in alloc
                      urb->transfer_buffer,        // CPU address
                      urb->transfer_dma);          // DMA address

    // Free the SKB (if we have a pointer to it)
    if (skb) {
        priv->tx_skb = NULL; // Clear pointer before freeing
        dev_kfree_skb_any(skb);
    }

    // Free the URB itself
    usb_free_urb(urb);

    // --- TX is no longer busy ---
    // Clear the busy flag *before* checking queue, ensures worker won't race
    atomic_set(&priv->tx_busy, 0);

    // Check if more packets are waiting and schedule worker if needed
    spin_lock_irqsave(&priv->tx_queue_lock, flags);
    queue_was_stopped = netif_queue_stopped(priv->net_dev);
    if (skb_queue_len(&priv->tx_queue) > 0) {
        // More work to do, queue the worker again
        queue_work(priv->tx_wq, &priv->tx_worker_work);
        work_queued = true; // Worker will handle waking queue if necessary later
    } else {
        // Queue is empty, wake it up if it was stopped
        if (queue_was_stopped) {
             netif_wake_queue(priv->net_dev);
             printk(KERN_DEBUG "%s: TX queue woken up by completion\n", priv->net_dev->name);
        }
    }
    spin_unlock_irqrestore(&priv->tx_queue_lock, flags);

}

// --- RX Completion Handler (runs in atomic context) ---
static void rtl8811au_rx_complete(struct urb *urb) {
    struct rtl8811au_dev *priv = urb->context;
    struct sk_buff *skb;
    int status = urb->status;
    int retval;
    struct net_device_stats *stats;
    unsigned long flags;

    // Basic sanity check
    if (!priv || !priv->net_dev) {
        printk(KERN_ERR "rtl8811au_wifi: Invalid context or net_dev in RX complete\n");
        return;
    }

    stats = &priv->net_dev->stats;

    // Handle based on URB status
    switch (status) {
    case 0: // Success
        // Reset error counter on success
        priv->rx_error_count = 0;

        // Check if we actually received data
        if (urb->actual_length == 0) {
            printk(KERN_DEBUG "%s: RX URB success but zero length\n", priv->net_dev->name);
            goto resubmit_rx; // Just resubmit the URB
        }

        // Allocate an SKB for the received data
        // Add headroom for potential later processing
        skb = dev_alloc_skb(urb->actual_length + NET_IP_ALIGN);
        if (skb) {
             // Ensure IP header will be aligned
            skb_reserve(skb, NET_IP_ALIGN);
            // Copy data from our DMA buffer to the skb
            memcpy(skb_put(skb, urb->actual_length), priv->rx_buffer, urb->actual_length);

            // Set up SKB metadata
            skb->dev = priv->net_dev;
            skb->protocol = eth_type_trans(skb, priv->net_dev);
            skb->ip_summed = CHECKSUM_NONE; // Assume no checksum offload

            // Send it up the network stack
            netif_rx(skb);

            // Update stats
            spin_lock_irqsave(&priv->stats_lock, flags);
            stats->rx_packets++;
            stats->rx_bytes += urb->actual_length;
            spin_unlock_irqrestore(&priv->stats_lock, flags);
        } else {
            printk(KERN_ERR "%s: Failed to allocate skb for RX (len %d)\n", priv->net_dev->name, urb->actual_length);
            spin_lock_irqsave(&priv->stats_lock, flags);
            stats->rx_dropped++;
            spin_unlock_irqrestore(&priv->stats_lock, flags);
            // Continue to resubmit URB even if skb allocation failed
        }
        break; // Go to resubmit

    // Handle errors that mean the device is gone or stopping
    case -ENOENT:      // URB killed
    case -ECONNRESET:   // URB unlinked
    case -ESHUTDOWN:    // Device shutdown
    case -ENODEV:       // Device removed
        printk(KERN_INFO "%s: RX URB cancelled (status %d), device stopping.\n", priv->net_dev->name, status);
        return; // Do not resubmit

    // Handle other errors
    default:
        priv->rx_error_count++; // Increment error counter
        printk(KERN_ERR "%s: RX URB failed (status %d, count %d)\n", priv->net_dev->name, status, priv->rx_error_count);
        spin_lock_irqsave(&priv->stats_lock, flags);
        stats->rx_errors++;
        spin_unlock_irqrestore(&priv->stats_lock, flags);

        // Check if we exceeded the consecutive error limit
        if (priv->rx_error_count > MAX_RX_ERRORS) {
            printk(KERN_CRIT "%s: Too many consecutive RX errors (%d). Stopping RX.\n", priv->net_dev->name, priv->rx_error_count);
            // TODO: Maybe notify higher layers or try a device reset?
            return; // Stop submitting RX URBs
        }

        // FIXED: Removed msleep(100) - sleeping is bad in atomic context.
        // If errors persist, the error counter will stop resubmission.
        // For transient errors (like -EPIPE sometimes), immediate retry might work.
        break; // Go to resubmit
    }

resubmit_rx:
    // Resubmit the URB for next packet (unless we returned due to fatal error/too many errors)
    // Re-use the same URB and buffer
    usb_fill_bulk_urb(priv->rx_urb, priv->usb_dev,
                      usb_rcvbulkpipe(priv->usb_dev, priv->bulk_in_endpoint),
                      priv->rx_buffer, MAX_PACKET_SIZE,
                      rtl8811au_rx_complete, priv);
    priv->rx_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
    priv->rx_urb->transfer_dma = priv->rx_dma;


    // Use GFP_ATOMIC since we are in interrupt context (completion handler)
    retval = usb_submit_urb(priv->rx_urb, GFP_ATOMIC);
    if (retval) {
        // Log error, increment stats, increment error count
        priv->rx_error_count++;
        printk(KERN_ERR "%s: Failed to resubmit RX URB (error %d, count %d)\n", priv->net_dev->name, retval, priv->rx_error_count);
        spin_lock_irqsave(&priv->stats_lock, flags);
        stats->rx_errors++;
        spin_unlock_irqrestore(&priv->stats_lock, flags);

        if (priv->rx_error_count > MAX_RX_ERRORS) {
             printk(KERN_CRIT "%s: Too many consecutive RX errors (%d) after failed resubmit. Stopping RX.\n", priv->net_dev->name, priv->rx_error_count);
             // Free resources here? No, stop should handle it.
        }
        // Don't loop trying to resubmit here if it fails.
    }
}

// --- Set MAC Address ---
static int rtl8811au_set_mac_address(struct net_device *dev, void *addr) {
    struct sockaddr *sa = addr;
    struct rtl8811au_dev *priv = netdev_priv(dev); // Get priv pointer

    if (!is_valid_ether_addr(sa->sa_data)) {
        return -EADDRNOTAVAIL;
    }

    // Copy address to net_device structure
    eth_hw_addr_set(dev, sa->sa_data);
    printk(KERN_INFO "%s: MAC address set to %pM\n", dev->name, dev->dev_addr);

    // TODO: Need to send command to hardware to set its MAC address filter
    // This requires device-specific control transfer knowledge.
    // Example: rtlwifi_set_mac_addr(priv->hw, sa->sa_data);

    return 0;
}

// --- Probe Function ---
static int rtl8811au_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct usb_device *usb_dev = interface_to_usbdev(interface);
    struct rtl8811au_dev *priv = NULL;
    struct wiphy *wiphy = NULL;
    struct ieee80211_supported_band *band_2g = NULL;
    struct ieee80211_rate *rates_2g = NULL;
    struct net_device *net_dev = NULL;
    int ret;
    int i; // Loop counter

    printk(KERN_INFO "rtl8811au_wifi: Probing device (Vendor: 0x%04x, Product: 0x%04x)\n", id->idVendor, id->idProduct);

    // Use devm_kzalloc for automatic cleanup on driver detach
    priv = devm_kzalloc(&interface->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        ret = -ENOMEM;
        dev_err(&interface->dev, "Failed to allocate device structure\n");
        return ret;
    }

    priv->usb_dev = usb_get_dev(usb_dev); // Increment refcount
    priv->usb_intf = interface;
    usb_set_intfdata(interface, priv); // Link interface to private data

    // --- Dynamically find bulk endpoints ---
    struct usb_host_interface *alt = interface->cur_altsetting;
    priv->bulk_in_endpoint = 0;
    priv->bulk_out_endpoint = 0;

    for (i = 0; i < alt->desc.bNumEndpoints; i++) {
        struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
        if (!priv->bulk_in_endpoint && usb_endpoint_is_bulk_in(ep)) {
            priv->bulk_in_endpoint = ep->bEndpointAddress;
            printk(KERN_INFO "rtl8811au_wifi: Found bulk IN endpoint: 0x%02x\n", priv->bulk_in_endpoint);
        }
        if (!priv->bulk_out_endpoint && usb_endpoint_is_bulk_out(ep)) {
            priv->bulk_out_endpoint = ep->bEndpointAddress;
            printk(KERN_INFO "rtl8811au_wifi: Found bulk OUT endpoint: 0x%02x\n", priv->bulk_out_endpoint);
        }
    }

    if (!priv->bulk_in_endpoint || !priv->bulk_out_endpoint) {
        dev_err(&interface->dev, "Could not find bulk IN/OUT endpoints\n");
        ret = -ENODEV;
        goto err_put_usb;
    }

    // Initialize spinlocks, queue, atomic variable
    // spin_lock_init(&priv->tx_lock); // Removed, unused
    spin_lock_init(&priv->tx_queue_lock);
    spin_lock_init(&priv->stats_lock);
    skb_queue_head_init(&priv->tx_queue);
    atomic_set(&priv->tx_busy, 0);
    // init_completion(&priv->tx_complete); // Removed, unused
    priv->tx_skb = NULL; // Initialize tx skb pointer

    // --- Request Firmware ---
    ret = request_firmware(&priv->firmware, RTL8811AU_FIRMWARE, &interface->dev);
    if (ret) {
        dev_err(&interface->dev, "Failed to request firmware %s (error %d)\n", RTL8811AU_FIRMWARE, ret);
        // Allow probe to continue without firmware? Maybe not for this device.
        goto err_put_usb;
    }
    printk(KERN_INFO "rtl8811au_wifi: Firmware %s loaded (%zu bytes)\n", RTL8811AU_FIRMWARE, priv->firmware->size);
    // TODO: Add code to actually *upload* and initialize the firmware on the device.
    // This is highly device-specific.

    // --- Allocate and Setup Wiphy (cfg80211 structure) ---
    // Note: wiphy is not device-managed, needs explicit freeing on error/disconnect
    wiphy = wiphy_new(&rtl8811au_cfg80211_ops, sizeof(struct rtl8811au_dev *)); // Store pointer back to priv
    if (!wiphy) {
        ret = -ENOMEM;
        dev_err(&interface->dev, "Failed to allocate wiphy\n");
        goto err_release_firmware;
    }
    // Store pointer to priv in wiphy private data (used by ops)
    *(struct rtl8811au_dev **)wiphy_priv(wiphy) = priv;
    priv->wiphy = wiphy; // Store wiphy pointer in our main priv structure

    set_wiphy_dev(wiphy, &interface->dev); // Associate wiphy with the USB interface device
    // wiphy->privid = some_unique_id; // Not strictly needed here

    // Define supported interface modes (e.g., Station)
    wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
    // TODO: Add other modes if supported (AP, Monitor, etc.)

    // --- Define Supported Bands/Channels/Rates ---
    // Allocate 2GHz band structure (use devm_ for interface-bound resources)
    band_2g = devm_kzalloc(&interface->dev, sizeof(*band_2g), GFP_KERNEL);
    if (!band_2g) {
        ret = -ENOMEM;
        dev_err(&interface->dev, "Failed to allocate band_2g\n");
        goto err_free_wiphy;
    }

    // Allocate channels (example: 1-14 for 2GHz)
    band_2g->n_channels = 14;
    band_2g->channels = devm_kcalloc(&interface->dev, band_2g->n_channels, sizeof(struct ieee80211_channel), GFP_KERNEL);
    if (!band_2g->channels) {
        ret = -ENOMEM;
        dev_err(&interface->dev, "Failed to allocate channels\n");
        // devm_kzalloc for band_2g handles its cleanup
        goto err_free_wiphy;
    }
    for (i = 0; i < band_2g->n_channels; i++) {
        band_2g->channels[i].band = NL80211_BAND_2GHZ;
        band_2g->channels[i].center_freq = ieee80211_channel_to_frequency(i + 1, NL80211_BAND_2GHZ);
        band_2g->channels[i].hw_value = i + 1;
        band_2g->channels[i].max_power = 20; // Example power limit (dBm)
        // TODO: Set channel flags (NO_IR, RADAR, etc.) based on region/device caps
    }

    // Allocate basic rates (802.11b/g) - Placeholder! AC600 needs much more.
    band_2g->n_bitrates = 7;
    rates_2g = devm_kcalloc(&interface->dev, band_2g->n_bitrates, sizeof(struct ieee80211_rate), GFP_KERNEL);
    if (!rates_2g) {
        ret = -ENOMEM;
        dev_err(&interface->dev, "Failed to allocate bitrates\n");
        goto err_free_wiphy; // devm handles channels/band
    }
    band_2g->bitrates = rates_2g;
    // Rates in 100kbps units
    rates_2g[0] = (struct ieee80211_rate){ .bitrate = 10, .hw_value = 0, .flags = IEEE80211_RATE_SHORT_PREAMBLE }; // 1 Mbps
    rates_2g[1] = (struct ieee80211_rate){ .bitrate = 20, .hw_value = 1, .flags = IEEE80211_RATE_SHORT_PREAMBLE }; // 2 Mbps
    rates_2g[2] = (struct ieee80211_rate){ .bitrate = 55, .hw_value = 2, .flags = IEEE80211_RATE_SHORT_PREAMBLE }; // 5.5 Mbps
    rates_2g[3] = (struct ieee80211_rate){ .bitrate = 110, .hw_value = 3, .flags = IEEE80211_RATE_SHORT_PREAMBLE };// 11 Mbps
    rates_2g[4] = (struct ieee80211_rate){ .bitrate = 60, .hw_value = 4 }; // 6 Mbps (OFDM)
    rates_2g[5] = (struct ieee80211_rate){ .bitrate = 120, .hw_value = 5 }; // 12 Mbps (OFDM)
    rates_2g[6] = (struct ieee80211_rate){ .bitrate = 240, .hw_value = 6 }; // 24 Mbps (OFDM)
    // TODO: Add 802.11n (HT) and 802.11ac (VHT) capabilities
    // band_2g->ht_cap = ...;
    // band_2g->vht_cap = ...;
    // TODO: Add 5GHz band definition (NL80211_BAND_5GHZ)

    wiphy->bands[NL80211_BAND_2GHZ] = band_2g;
    // wiphy->bands[NL80211_BAND_5GHZ] = band_5g; // If defined

    // --- Allocate and Setup Netdevice ---
    // Use alloc_etherdev for standard Ethernet setup
    net_dev = alloc_etherdev(sizeof(struct rtl8811au_dev));
    if (!net_dev) {
        ret = -ENOMEM;
        dev_err(&interface->dev, "Failed to allocate network device\n");
        goto err_free_wiphy; // devm handles bands/rates/channels
    }
    priv->net_dev = net_dev; // Link net_dev in our priv struct
    // Copy needed info from interface's priv to net_dev's priv
    // netdev_priv gets the area allocated by alloc_etherdev
    struct rtl8811au_dev *netdev_priv_data = netdev_priv(net_dev);
    memcpy(netdev_priv_data, priv, sizeof(*priv)); // Copy basic pointers (usb_dev, endpoints, etc.)

    SET_NETDEV_DEV(net_dev, &interface->dev); // Associate net_dev with USB interface device
    net_dev->netdev_ops = &rtl8811au_netdev_ops; // Assign network operations
    // Assign wireless extensions pointer (legacy, but some tools might use it)
    // net_dev->wireless_handlers = &rtl8811au_whandler_def;
    // Assign cfg80211 pointer
    net_dev->ieee80211_ptr = &rtl8811au_wdev;
    rtl8811au_wdev.wiphy = wiphy; // Link wireless_dev to wiphy
    // rtl8811au_wdev.netdev = net_dev; // Set back pointer if needed by wdev ops

    // Set a random MAC address initially. Should be read from device later.
    eth_hw_addr_random(net_dev);
    printk(KERN_INFO "rtl8811au_wifi: Assigned random MAC %pM\n", net_dev->dev_addr);
    // TODO: Read MAC from hardware EEPROM/OTP and use it instead.

    // --- Initialize TX Workqueue ---
    // Create workqueue after net_dev is set up
    priv->tx_wq = create_singlethread_workqueue(net_dev->name);
    if (!priv->tx_wq) {
        dev_err(&interface->dev, "Failed to create TX workqueue\n");
        ret = -ENOMEM;
        goto err_free_netdev;
    }
    INIT_WORK(&priv->tx_worker_work, rtl8811au_tx_worker);

    // --- Register Wiphy and Netdevice ---
    ret = wiphy_register(wiphy);
    if (ret < 0) {
        dev_err(&interface->dev, "wiphy_register failed (%d)\n", ret);
        goto err_destroy_wq;
    }
    printk(KERN_INFO "rtl8811au_wifi: wiphy registered\n");
    // wiphy registration succeeded, ownership transferred to cfg80211
    // Do not call wiphy_free in error path beyond this point if register succeeds.

    ret = register_netdev(net_dev);
    if (ret) {
        dev_err(&interface->dev, "register_netdev failed (%d)\n", ret);
        goto err_unregister_wiphy; // Cleanup wiphy registration
    }
    printk(KERN_INFO "rtl8811au_wifi: netdev %s registered\n", net_dev->name);

    printk(KERN_INFO "rtl8811au_wifi: Probe successful for %s\n", net_dev->name);
    return 0; // Success

// --- Error Handling Cleanup ---
err_unregister_wiphy:
    wiphy_unregister(wiphy);
    // Fall through to destroy WQ
err_destroy_wq:
    if (priv->tx_wq) {
        destroy_workqueue(priv->tx_wq);
    }
    // Fall through to free netdev
err_free_netdev:
    free_netdev(net_dev); // Free net_device allocated with alloc_etherdev
    // Fall through to free wiphy
err_free_wiphy:
    wiphy_free(wiphy); // Free wiphy if registration failed or didn't happen
    // Fall through to release firmware
err_release_firmware:
    release_firmware(priv->firmware);
    // Fall through to put USB device ref
err_put_usb:
    usb_set_intfdata(interface, NULL); // Clear association
    usb_put_dev(usb_dev); // Decrement refcount
    // devm_kzalloc for priv will be cleaned up automatically

    printk(KERN_ERR "rtl8811au_wifi: Probe failed with error %d\n", ret);
    return ret;
}

// --- Disconnect Function ---
static void rtl8811au_disconnect(struct usb_interface *interface) {
    // Get private data structure back from interface
    struct rtl8811au_dev *priv = usb_get_intfdata(interface);

    if (!priv) {
        printk(KERN_INFO "rtl8811au_wifi: Disconnect called on non-probed interface?\n");
        return;
    }

    printk(KERN_INFO "rtl8811au_wifi: Disconnecting device %s\n", priv->net_dev ? priv->net_dev->name : "(null net_dev)");

    // Unregister netdevice first (stops traffic, calls ndo_stop)
    if (priv->net_dev) {
        unregister_netdev(priv->net_dev);
        // Note: free_netdev is called automatically by unregister_netdev
        // if the device refcount drops to zero (which it should here).
        priv->net_dev = NULL; // Clear our pointer
    }

    // Unregister wiphy
    if (priv->wiphy) {
        wiphy_unregister(priv->wiphy);
        wiphy_free(priv->wiphy); // Free wiphy structure
        priv->wiphy = NULL; // Clear our pointer
    }

    // Clean up TX workqueue (FIXED: Moved here from stop)
    if (priv->tx_wq) {
        cancel_work_sync(&priv->tx_worker_work); // Ensure TX worker isn't running
        destroy_workqueue(priv->tx_wq); // Destroy the workqueue
        priv->tx_wq = NULL;
    }

    // RX URB/buffer cleanup happens in ndo_stop, which is called by unregister_netdev
    // Just ensure pointers are null if stop wasn't called for some reason.
    if (priv->rx_urb) {
       usb_kill_urb(priv->rx_urb);
       usb_free_urb(priv->rx_urb);
       priv->rx_urb = NULL;
    }
    if (priv->rx_buffer) {
        usb_free_coherent(priv->usb_dev, MAX_PACKET_SIZE, priv->rx_buffer, priv->rx_dma);
        priv->rx_buffer = NULL;
    }


    // Release firmware
    if (priv->firmware) {
        release_firmware(priv->firmware);
        priv->firmware = NULL;
    }

    // Cleanup remaining USB resources
    usb_set_intfdata(interface, NULL); // Clear association
    if (priv->usb_dev) {
        usb_put_dev(priv->usb_dev); // Decrement refcount taken in probe
        priv->usb_dev = NULL;
    }

    // devm_kzalloc'd memory (band, channels, rates, main priv) is freed automatically

    printk(KERN_INFO "rtl8811au_wifi: Device disconnected\n");
}

// --- USB Driver ---
static struct usb_driver rtl8811au_driver = {
    .name = "rtl8811au_wifi", // Driver name
    .id_table = rtl8811au_table, // USB IDs it supports
    .probe = rtl8811au_probe, // Probe function
    .disconnect = rtl8811au_disconnect, // Disconnect function
    // Add suspend/resume handlers if power management is needed
    // .suspend = rtl8811au_suspend,
    // .resume = rtl8811au_resume,
};

// --- Module Init/Exit ---
static int __init rtl8811au_init(void) {
    int ret;
    printk(KERN_INFO "rtl8811au_wifi: Initializing driver...\n");
    ret = usb_register(&rtl8811au_driver);
    if (ret) {
        printk(KERN_ERR "rtl8811au_wifi: usb_register failed (error %d)\n", ret);
        return ret;
    }
    printk(KERN_INFO "rtl8811au_wifi: Driver registered successfully.\n");
    return 0;
}

static void __exit rtl8811au_exit(void) {
    printk(KERN_INFO "rtl8811au_wifi: Exiting driver...\n");
    usb_deregister(&rtl8811au_driver);
    printk(KERN_INFO "rtl8811au_wifi: Driver deregistered.\n");
}

module_init(rtl8811au_init);
module_exit(rtl8811au_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("pseudo-software-inc (with fixes by AI)");
MODULE_DESCRIPTION("Basic RTL8811AU Wi-Fi USB driver skeleton");
MODULE_VERSION("0.2"); // Indicate version with fixes