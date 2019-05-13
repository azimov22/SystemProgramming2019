#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/kref.h>
#include <asm/uaccess.h>

#define MINOR_BASE	192
#define to_skel_dev(d) container_of(d, struct usb_skel, kref)

static struct usb_driver skel_driver;

// USB skeleton struct
struct usb_skel {
	struct usb_device *	udev;
	struct usb_interface *	interface;
	unsigned char *		bulk_in_buffer;		// The buffer to receive data
	size_t			bulk_in_size;		    // The size of the receive buffer
	__u8			bulk_in_endpointAddr;	// The address of the bulk in endpoint
	__u8			bulk_out_endpointAddr;	// The address of the bulk out endpoint
	struct kref		kref;
};

// Buffer cleaner
static const void skel_write_bulk_callback(struct urb *urb, struct pt_regs *regs)
{
	// if (urb->status && 
	//     !(urb->status == -ENOENT || 
	//       urb->status == -ECONNRESET ||
	//       urb->status == -ESHUTDOWN)) {
	// 	printk(KERN_INFO "%s - nonzero write bulk status received: %d",
	// 	    __FUNCTION__, urb->status);
	// }

	// Free up our allocated buffer
	usb_free_coherent(urb->dev, urb->transfer_buffer_length, 
			urb->transfer_buffer, urb->transfer_dma);
}

// Write File to a device function
static ssize_t dev_write(struct file *file, const char __user *user_buffer, size_t count, loff_t *ppos)
{
	struct usb_skel *device_skeleton;
	int return_output = 0;
	struct urb *urb = NULL;
	char *buf = NULL;

	device_skeleton = (struct usb_skel *)file->private_data;

	/* verify that we actually have some data to write */
	if (count == 0)
		return 0;

	/* create a urb, and a buffer for it, and copy the data to the urb */
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		return_output = -ENOMEM;
		return return_output;
	}

	buf = usb_alloc_coherent(device_skeleton->udev, count, GFP_KERNEL, &urb->transfer_dma);
	if (!buf) {
		return_output = -ENOMEM;
		return return_output;
	}
	if (copy_from_user(buf, user_buffer, count)) {
		return_output = -EFAULT;
		return return_output;
	}

	/* initialize the urb properly */
	// usb_fill_bulk_urb(urb, device_skeleton->udev, usb_sndbulkpipe(device_skeleton->udev, device_skeleton->bulk_out_endpointAddr),
	    // buf, count, *skel_write_bulk_callback, device_skeleton);

	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* send the data out the bulk port */
	return_output = usb_submit_urb(urb, GFP_KERNEL);
	if (return_output) {
		printk(KERN_INFO "%s - failed submitting write urb, error %d", __FUNCTION__, return_output);
		return return_output;
	}

	/* release our reference to this urb, the USB core will eventually free it entirely */
	usb_free_urb(urb);

    return count;
}

// File operations for a device
static struct file_operations operations = {
	.owner =	THIS_MODULE,
	// .read =		dev_read,
	.write =	dev_write,
	// .open =		dev_open,
	// .release =	dev_release,
};

static struct usb_class_driver ucd = {
	.name = "usb/skel%d",
	.fops = &operations,
	.minor_base = MINOR_BASE,
};

// Output for usb connect interruption
static int dev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
    struct usb_skel *device_skeleton = NULL;
	struct usb_host_interface *iface_descriptor;
	struct usb_endpoint_descriptor *endpoint;
	size_t buffer_size;
    int return_output = -ENOMEM;
    int i;

    // Device skeleton memory allocation
    device_skeleton = kzalloc(sizeof(struct usb_skel), GFP_KERNEL);
	if (device_skeleton == NULL) {
		printk(KERN_INFO "Out of memory");
    }
    memset(device_skeleton, 0x00, sizeof (*device_skeleton));
    kref_init(&device_skeleton->kref);

    device_skeleton->udev = usb_get_dev(interface_to_usbdev(interface));
	device_skeleton->interface = interface;
    iface_descriptor = interface->cur_altsetting;

    // Endpoint memory allocation and declaration
    for(i = 0; i < iface_descriptor->desc.bNumEndpoints; i++){
        endpoint = &iface_descriptor->endpoint[i].desc;
        
        if((!device_skeleton->bulk_in_endpointAddr) && (endpoint->bEndpointAddress & USB_DIR_IN) &&
            ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)){
            buffer_size = endpoint->wMaxPacketSize;
            device_skeleton->bulk_in_size = buffer_size;
            device_skeleton->bulk_in_endpointAddr = endpoint->bEndpointAddress;
            device_skeleton->bulk_in_buffer = kmalloc(buffer_size, GFP_KERNEL);

            // Error in bulk in buffer
            if (!device_skeleton->bulk_in_buffer) {
                printk(KERN_INFO "Could not allocate bulk_in_buffer");
            }
        
        }

        if (!device_skeleton->bulk_out_endpointAddr && !(endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) == USB_ENDPOINT_XFER_BULK)) {
			
            // There is an endpoint
			device_skeleton->bulk_out_endpointAddr = endpoint->bEndpointAddress;
		}
        
    }

    if (!(device_skeleton->bulk_in_endpointAddr && device_skeleton->bulk_out_endpointAddr)) {
        printk(KERN_INFO "Could not find both bulk-in and bulk-out endpoints");
	}

	// Save to pointer
	usb_set_intfdata(interface, device_skeleton);

	// Device registration
	return_output = usb_register_dev(interface, &ucd);

	if (return_output) {

		// Register device error
		printk(KERN_INFO "Not able to get a minor for this device.");
		usb_set_intfdata(interface, NULL);
	}

	printk(KERN_INFO "USB Skeleton device now attached to USBSkel-%d", interface->minor);
    printk(KERN_INFO "Dev drive (%04X:%04X) plugged\n", id->idVendor, id->idProduct);
	return 0;
    
}

// Output for usb disconnect interruption
static void dev_disconnect(struct usb_interface *interface)
{
    printk(KERN_INFO "Dev drive removed\n");
}

// Hardcoded device table to check which device is plugged (on which device we should interrupt)
static struct usb_device_id dev_table[] =
{
    { USB_DEVICE(0x04e8, 0x6860) }, // Hardcoded usb device vendor and products id
    {}
};
MODULE_DEVICE_TABLE (usb, dev_table);

// device structure
static struct usb_driver dev_driver =
{
    // .owner = THIS_MODULE,
    .name = "Mobile USB driver",
    .id_table = dev_table,
    .probe = dev_probe,
    .disconnect = dev_disconnect,
};

// start of module
static int __init dev_init(void)
{
    int result = usb_register(&dev_driver);
    if(result){
        printk(KERN_INFO "Usb device register failed. Error: %d\n", result);
    }
    return result;
}
 
// module destructor
static void __exit dev_exit(void)
{
    usb_deregister(&dev_driver);
    printk(KERN_INFO "Shutting down the driver");
}

// modules initialization
module_init(dev_init);
module_exit(dev_exit);

MODULE_LICENSE("GPL");