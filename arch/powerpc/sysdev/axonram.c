/*
 * (C) Copyright IBM Deutschland Entwicklung GmbH 2006
 *
 * Author: Maxim Shchetynin <maxim@de.ibm.com>
 *
 * Axon DDR2 device driver.
 * It registers one block device per Axon's DDR2 memory bank found on a system.
 * Block devices are called axonram?, their major and minor numbers are
 * available in /proc/devices, /proc/partitions or in /sys/block/axonram?/dev.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irq.h>
#include <linux/irqreturn.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/of_device.h>
#include <asm/of_platform.h>
#include <asm/page.h>
#include <asm/prom.h>

#define AXON_RAM_MODULE_NAME		"axonram"
#define AXON_RAM_DEVICE_NAME		"axonram"
#define AXON_RAM_MINORS_PER_DISK	16
#define AXON_RAM_BLOCK_SHIFT		PAGE_SHIFT
#define AXON_RAM_BLOCK_SIZE		1 << AXON_RAM_BLOCK_SHIFT
#define AXON_RAM_SECTOR_SHIFT		9
#define AXON_RAM_SECTOR_SIZE		1 << AXON_RAM_SECTOR_SHIFT
#define AXON_RAM_IRQ_FLAGS		IRQF_SHARED | IRQF_TRIGGER_RISING

struct axon_ram_bank {
	struct of_device	*device;
	struct gendisk		*disk;
	unsigned int		irq_correctable;
	unsigned int		irq_uncorrectable;
	unsigned long		ph_addr;
	unsigned long		io_addr;
	unsigned long		size;
	unsigned long		ecc_counter;
};

static ssize_t
axon_ram_sysfs_ecc(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct of_device *device = to_of_device(dev);
	struct axon_ram_bank *bank = device->dev.platform_data;

	BUG_ON(!bank);

	return sprintf(buf, "%ld\n", bank->ecc_counter);
}

static DEVICE_ATTR(ecc, S_IRUGO, axon_ram_sysfs_ecc, NULL);

/**
 * axon_ram_irq_handler - interrupt handler for Axon RAM ECC
 * @irq: interrupt ID
 * @dev: pointer to of_device
 */
static irqreturn_t
axon_ram_irq_handler(int irq, void *dev)
{
	struct of_device *device = dev;
	struct axon_ram_bank *bank = device->dev.platform_data;

	BUG_ON(!bank);

	if (irq == bank->irq_correctable) {
		dev_err(&device->dev, "Correctable memory error occured\n");
		bank->ecc_counter++;
		return IRQ_HANDLED;
	} else if (irq == bank->irq_uncorrectable) {
		dev_err(&device->dev, "Uncorrectable memory error occured\n");
		panic("Critical ECC error on %s", device->node->full_name);
	}

	return IRQ_NONE;
}

/**
 * axon_ram_make_request - make_request() method for block device
 * @queue, @bio: see blk_queue_make_request()
 */
static int
axon_ram_make_request(struct request_queue *queue, struct bio *bio)
{
	struct axon_ram_bank *bank = bio->bi_bdev->bd_disk->private_data;
	unsigned long phys_mem, phys_end;
	void *user_mem;
	struct bio_vec *vec;
	unsigned int transfered;
	unsigned short idx;
	int rc = 0;

	phys_mem = bank->io_addr + (bio->bi_sector << AXON_RAM_SECTOR_SHIFT);
	phys_end = bank->io_addr + bank->size;
	transfered = 0;
	bio_for_each_segment(vec, bio, idx) {
		if (unlikely(phys_mem + vec->bv_len > phys_end)) {
			bio_io_error(bio, bio->bi_size);
			rc = -ERANGE;
			break;
		}

		user_mem = page_address(vec->bv_page) + vec->bv_offset;
		if (bio_data_dir(bio) == READ)
			memcpy(user_mem, (void *) phys_mem, vec->bv_len);
		else
			memcpy((void *) phys_mem, user_mem, vec->bv_len);

		phys_mem += vec->bv_len;
		transfered += vec->bv_len;
	}
	bio_endio(bio, transfered, 0);

	return rc;
}

/**
 * axon_ram_direct_access - direct_access() method for block device
 * @device, @sector, @data: see block_device_operations method
 */
static int
axon_ram_direct_access(struct block_device *device, sector_t sector,
		       unsigned long *data)
{
	struct axon_ram_bank *bank = device->bd_disk->private_data;
	loff_t offset;

	offset = sector << AXON_RAM_SECTOR_SHIFT;
	if (offset >= bank->size) {
		dev_err(&bank->device->dev, "Access outside of address space\n");
		return -ERANGE;
	}

	*data = bank->ph_addr + offset;

	return 0;
}

static struct block_device_operations axon_ram_devops = {
	.owner		= THIS_MODULE,
	.direct_access	= axon_ram_direct_access
};

/**
 * axon_ram_probe - probe() method for platform driver
 * @device, @device_id: see of_platform_driver method
 */
static int
axon_ram_probe(struct of_device *device, const struct of_device_id *device_id)
{
	static int axon_ram_bank_id = -1;
	struct axon_ram_bank *bank;
	struct resource resource;
	int rc = 0;

	axon_ram_bank_id++;

	dev_info(&device->dev, "Found memory controller on %s\n",
			device->node->full_name);

	bank = kzalloc(sizeof(struct axon_ram_bank), GFP_KERNEL);
	if (bank == NULL) {
		dev_err(&device->dev, "Out of memory\n");
		rc = -ENOMEM;
		goto failed;
	}

	device->dev.platform_data = bank;

	bank->device = device;

	if (of_address_to_resource(device->node, 0, &resource) != 0) {
		dev_err(&device->dev, "Cannot access device tree\n");
		rc = -EFAULT;
		goto failed;
	}

	bank->size = resource.end - resource.start + 1;

	if (bank->size == 0) {
		dev_err(&device->dev, "No DDR2 memory found for %s%d\n",
				AXON_RAM_DEVICE_NAME, axon_ram_bank_id);
		rc = -ENODEV;
		goto failed;
	}

	dev_info(&device->dev, "Register DDR2 memory device %s%d with %luMB\n",
			AXON_RAM_DEVICE_NAME, axon_ram_bank_id, bank->size >> 20);

	bank->ph_addr = resource.start;
	bank->io_addr = (unsigned long) ioremap_flags(
			bank->ph_addr, bank->size, _PAGE_NO_CACHE);
	if (bank->io_addr == 0) {
		dev_err(&device->dev, "ioremap() failed\n");
		rc = -EFAULT;
		goto failed;
	}

	bank->disk = alloc_disk(AXON_RAM_MINORS_PER_DISK);
	if (bank->disk == NULL) {
		dev_err(&device->dev, "Cannot register disk\n");
		rc = -EFAULT;
		goto failed;
	}

	bank->disk->first_minor = 0;
	bank->disk->fops = &axon_ram_devops;
	bank->disk->private_data = bank;
	bank->disk->driverfs_dev = &device->dev;

	sprintf(bank->disk->disk_name, "%s%d",
			AXON_RAM_DEVICE_NAME, axon_ram_bank_id);
	bank->disk->major = register_blkdev(0, bank->disk->disk_name);
	if (bank->disk->major < 0) {
		dev_err(&device->dev, "Cannot register block device\n");
		rc = -EFAULT;
		goto failed;
	}

	bank->disk->queue = blk_alloc_queue(GFP_KERNEL);
	if (bank->disk->queue == NULL) {
		dev_err(&device->dev, "Cannot register disk queue\n");
		rc = -EFAULT;
		goto failed;
	}

	set_capacity(bank->disk, bank->size >> AXON_RAM_SECTOR_SHIFT);
	blk_queue_make_request(bank->disk->queue, axon_ram_make_request);
	blk_queue_hardsect_size(bank->disk->queue, AXON_RAM_SECTOR_SIZE);
	add_disk(bank->disk);

	bank->irq_correctable = irq_of_parse_and_map(device->node, 0);
	bank->irq_uncorrectable = irq_of_parse_and_map(device->node, 1);
	if ((bank->irq_correctable <= 0) || (bank->irq_uncorrectable <= 0)) {
		dev_err(&device->dev, "Cannot access ECC interrupt ID\n");
		rc = -EFAULT;
		goto failed;
	}

	rc = request_irq(bank->irq_correctable, axon_ram_irq_handler,
			AXON_RAM_IRQ_FLAGS, bank->disk->disk_name, device);
	if (rc != 0) {
		dev_err(&device->dev, "Cannot register ECC interrupt handler\n");
		bank->irq_correctable = bank->irq_uncorrectable = 0;
		rc = -EFAULT;
		goto failed;
	}

	rc = request_irq(bank->irq_uncorrectable, axon_ram_irq_handler,
			AXON_RAM_IRQ_FLAGS, bank->disk->disk_name, device);
	if (rc != 0) {
		dev_err(&device->dev, "Cannot register ECC interrupt handler\n");
		bank->irq_uncorrectable = 0;
		rc = -EFAULT;
		goto failed;
	}

	rc = device_create_file(&device->dev, &dev_attr_ecc);
	if (rc != 0) {
		dev_err(&device->dev, "Cannot create sysfs file\n");
		rc = -EFAULT;
		goto failed;
	}

	return 0;

failed:
	if (bank != NULL) {
		if (bank->irq_uncorrectable > 0)
			free_irq(bank->irq_uncorrectable, device);
		if (bank->irq_correctable > 0)
			free_irq(bank->irq_correctable, device);
		if (bank->disk != NULL) {
			if (bank->disk->queue != NULL)
				blk_cleanup_queue(bank->disk->queue);
			if (bank->disk->major > 0)
				unregister_blkdev(bank->disk->major,
						bank->disk->disk_name);
			del_gendisk(bank->disk);
		}
		device->dev.platform_data = NULL;
		if (bank->io_addr != 0)
			iounmap((void __iomem *) bank->io_addr);
		kfree(bank);
	}

	return rc;
}

/**
 * axon_ram_remove - remove() method for platform driver
 * @device: see of_platform_driver method
 */
static int
axon_ram_remove(struct of_device *device)
{
	struct axon_ram_bank *bank = device->dev.platform_data;

	BUG_ON(!bank || !bank->disk);

	device_remove_file(&device->dev, &dev_attr_ecc);
	free_irq(bank->irq_uncorrectable, device);
	free_irq(bank->irq_correctable, device);
	blk_cleanup_queue(bank->disk->queue);
	unregister_blkdev(bank->disk->major, bank->disk->disk_name);
	del_gendisk(bank->disk);
	iounmap((void __iomem *) bank->io_addr);
	kfree(bank);

	return 0;
}

static struct of_device_id axon_ram_device_id[] = {
	{
		.type	= "dma-memory"
	},
	{}
};

static struct of_platform_driver axon_ram_driver = {
	.owner		= THIS_MODULE,
	.name		= AXON_RAM_MODULE_NAME,
	.match_table	= axon_ram_device_id,
	.probe		= axon_ram_probe,
	.remove		= axon_ram_remove
};

/**
 * axon_ram_init
 */
static int __init
axon_ram_init(void)
{
	return of_register_platform_driver(&axon_ram_driver);
}

/**
 * axon_ram_exit
 */
static void __exit
axon_ram_exit(void)
{
	of_unregister_platform_driver(&axon_ram_driver);
}

module_init(axon_ram_init);
module_exit(axon_ram_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maxim Shchetynin <maxim@de.ibm.com>");
MODULE_DESCRIPTION("Axon DDR2 RAM device driver for IBM Cell BE");