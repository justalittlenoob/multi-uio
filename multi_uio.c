// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/uio_driver.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>

#define DRIVER_LICENSE "GPL v2"
#define DRIVER_VERSION "0.0.3"
#define DRIVER_AUTHOR "Zhao Pengfei <pengfei.zhao@intel.com>"
#define DRIVER_DESC "Multi UIO driver based on IVSHMEM"

#define IVSHMEM_VENDOR_ID 0x1af4
#define IVSHMEM_DEVICE_ID 0x1110

#define BAR0 0
#define BAR2 2

#define MAX_UIO_PER_DEV 5
#define MAX_IVSHMEM_REGION 10

static char *region_config = "";
module_param(region_config, charp, 0444);
MODULE_PARM_DESC(region_config, 
        "region config, format: region_id:num_uios,region_id:num_uios,...");

struct ivshmem_region_config {
        int region_id;
        int num_uios;
};

static struct ivshmem_region_config configs[MAX_IVSHMEM_REGION];
static int num_configs;

struct ivshmem_uio {
        struct uio_info *uinfo;
        unsigned long bar2_offset;
};

struct ivshmem {
        struct ivshmem_uio *uios;
        struct pci_dev *pdev;
        int num_uios;
};

static irqreturn_t irqhandler(int irq, struct uio_info *uinfo)
{
        struct ivshmem *ivshmem = uinfo->priv;

        if (ivshmem->pdev->msix_enabled)
                return IRQ_HANDLED;

        void __iomem *plx_intscr = uinfo->mem[0].internal_addr + 0x04;
        u32 val = readl(plx_intscr);

        if ((val & 0x1) == 0)
                return IRQ_NONE;

        return IRQ_HANDLED;
}

static int parse_region_config(void)
{
        char *config_copy, *entry;
        int i = 0, region_id, num_uios;
        const char *delim = ",";

        // Allow empty config
        if (!region_config || strlen(region_config) == 0) {
                pr_info("multi_uio: No region_config provided,"
                        "use default config\n");
                return 0;
        }

        // Copy param string
        config_copy = kstrdup(region_config, GFP_KERNEL);
        if (!config_copy) {
                pr_err("multi_uio: Copy param string fail\n");
                return -ENOMEM;
        }

        // Separate configuration items with commas(",")
        entry = strsep(&config_copy, delim);
        while (entry && i < ARRAY_SIZE(configs)) {
                char *colon, *dev_str, *num_str;

                // Skip blank characters
                while (*entry == ' ') entry++;

                // Find colon(":") delimiter
                colon = strchr(entry, ':');
                if (!colon) {
                        pr_err("multi_uio: Invalid configuration item (missing" 
                                "colon): %s\n", entry);
                        goto error;
                }

                // Extract region id and UIO quantity
                *colon = '\0';      // Replace colon with string terminator
                dev_str = entry;
                num_str = colon + 1;

                // Convert to integer
                if (kstrtoint(dev_str, 10, &region_id) || 
                        kstrtoint(num_str, 10, &num_uios)) {
                        pr_err("multi_uio: Invalid numerical format: %s:%s\n", 
                                dev_str, num_str);
                        goto error;
                }

                // Verification scope
                if (num_uios < 1 || num_uios > MAX_UIO_PER_DEV) {
                        pr_err("multi_uio: UIO quantity exceeds the range(1-%d): "
                                "%d\n", MAX_UIO_PER_DEV, 
                                num_uios);
                        goto error;
                }

                // Save Configuration
                configs[i].region_id = region_id;
                configs[i].num_uios = num_uios;
                i++;

                // Process the next configuration item
                entry = strsep(&config_copy, delim);
        }

        num_configs = i;
        kfree(config_copy);
        return 0;

error:
        kfree(config_copy);
        return -EINVAL;
}

static int probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
        struct ivshmem *ivshmem;
        unsigned long bar0_addr, bar2_addr, bar2_size;
        int num_uios = 1, i, region_id = pdev->subsystem_device;

        for (i = 0; i < num_configs; i++) {
                pr_info("multi_uio: config region id: %d, subdev: %d\n", 
                        configs[i].region_id, region_id);
                if (configs[i].region_id == region_id) {
                        num_uios = configs[i].num_uios;
                        break;
                }
        }

        ivshmem = kzalloc(sizeof(struct ivshmem), GFP_KERNEL);
        ivshmem->uios = kcalloc(num_uios, sizeof(struct ivshmem_uio), 
                                GFP_KERNEL);
        ivshmem->num_uios = num_uios;
        ivshmem->pdev = pdev;

        if (pci_enable_device(pdev) || pci_request_regions(pdev, "ivshmem")) {
                dev_err(&pdev->dev, "multi_uio: PCI init fail\n");
                goto error;
        }

        bar0_addr = pci_resource_start(pdev, BAR0);
        bar2_addr = pci_resource_start(pdev, BAR2);
        bar2_size = (pci_resource_len(pdev, BAR2) + PAGE_SIZE - 1) & PAGE_MASK;

        if (pci_alloc_irq_vectors(pdev, num_uios, num_uios, PCI_IRQ_MSIX) < 
                num_uios) {
                dev_err(&pdev->dev, "multi_uio: Alloc %d irq vectors fail\n", 
                        num_uios);
                goto error_release;
        }

        for (i = 0; i < num_uios; i++) {
                struct ivshmem_uio *uio = &ivshmem->uios[i];
                uio->uinfo = kzalloc(sizeof(struct uio_info), GFP_KERNEL);
                uio->uinfo->name = kasprintf(GFP_KERNEL, "ivshmem%d", i);
                uio->uinfo->version = DRIVER_VERSION;
                uio->uinfo->priv = ivshmem;

                uio->uinfo->mem[0].memtype = UIO_MEM_PHYS;
                uio->uinfo->mem[0].name = "registers";
                uio->uinfo->mem[0].addr = bar0_addr;
                uio->uinfo->mem[0].size = (pci_resource_len(pdev, BAR0) + 
                                        PAGE_SIZE - 1) & PAGE_MASK;
                uio->uinfo->mem[0].internal_addr = pci_ioremap_bar(pdev, BAR0);

                uio->uinfo->mem[1].memtype = UIO_MEM_PHYS;
                uio->uinfo->mem[1].name = "shmem";
                uio->bar2_offset = i * (bar2_size / num_uios);
                uio->uinfo->mem[1].addr = bar2_addr + uio->bar2_offset;
                uio->uinfo->mem[1].size = bar2_size / num_uios;

                uio->uinfo->irq = pci_irq_vector(pdev, i);
                uio->uinfo->irq_flags = 0;
                uio->uinfo->handler = irqhandler;

                if (uio_register_device(&pdev->dev, uio->uinfo)) {
                        dev_err(&pdev->dev, "multi_uio: Register UIO dev %d"
                                "fail\n", i);
                        goto error_uio;
                }
        }

        pci_set_master(pdev);
        pci_set_drvdata(pdev, ivshmem);
        return 0;

error_uio:
        while (--i >= 0) {
                uio_unregister_device(ivshmem->uios[i].uinfo);
                kfree(ivshmem->uios[i].uinfo->name);
                kfree(ivshmem->uios[i].uinfo);
        }
error_release:
        pci_release_regions(pdev);
        pci_disable_device(pdev);
error:
        kfree(ivshmem->uios);
        kfree(ivshmem);
        return -ENODEV;
}

static void remove(struct pci_dev *pdev) {
        struct ivshmem *ivshmem = pci_get_drvdata(pdev);

        for (int i = 0; i < ivshmem->num_uios; i++) {
                struct ivshmem_uio *uio = &ivshmem->uios[i];
                uio_unregister_device(uio->uinfo);
                iounmap(uio->uinfo->mem[0].internal_addr);
                kfree(uio->uinfo->name);
                kfree(uio->uinfo);
        }

        pci_free_irq_vectors(pdev);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        kfree(ivshmem->uios);
        kfree(ivshmem);
}

static struct pci_device_id ivshmem_ids[] = {
        { PCI_DEVICE(IVSHMEM_VENDOR_ID, IVSHMEM_DEVICE_ID) },
        { 0, }
};
    
static struct pci_driver ivshmem_driver = {
        .name = "multi_uio_drv",
        .id_table = ivshmem_ids,
        .probe = probe,
        .remove = remove,
};

static int __init ivshmem_init(void)
{
        int ret = parse_region_config();

        if (ret)
                return ret;

        return pci_register_driver(&ivshmem_driver);
}

static void __exit ivshmem_exit(void)
{
    pci_unregister_driver(&ivshmem_driver);
}

module_init(ivshmem_init);
module_exit(ivshmem_exit);

MODULE_DEVICE_TABLE(pci, ivshmem_ids);
MODULE_LICENSE(DRIVER_LICENSE);
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);