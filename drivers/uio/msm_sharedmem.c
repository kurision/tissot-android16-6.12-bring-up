// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2026, The tissot kgsl-port authors.
 *
 * Port of the donor's drivers/uio/msm_sharedmem/msm_sharedmem.c.
 *
 * The ROM's rmt_storage daemon serves the modem's EFS partitions out of a
 * memory region shared with the modem. It finds the region as the UIO
 * device whose name is "rmtfs", mmaps map 0, and hands the physical
 * address of map 0 to the modem. Without it the daemon stops with
 * "Mmap Failed for rmts (-1)...pausing execution!", and the modem's
 * initialisation stalls until its watchdog fires ("Watchdog detects
 * stalled initialization").
 *
 * The UIO name is the "reg-names" entry of the region, as on the donor.
 * A region at address 0 is allocated at probe, as the donor did; a region
 * with an address is used as it is.
 *
 * The donor also assigned the region to the modem's VM with
 * hyp_assign_phys(). msm8953 runs no hypervisor: the upstream msm8953
 * rmtfs region carries no qcom,vmid, and the donor's own code treats an
 * unsupported assignment as a warning. So nothing is assigned here.
 */

#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>

static int sharedmem_mmap(struct uio_info *info, struct vm_area_struct *vma)
{
	struct uio_mem *mem;

	if (vma->vm_pgoff >= MAX_UIO_MAPS || !info->mem[vma->vm_pgoff].size)
		return -EINVAL;

	mem = &info->mem[vma->vm_pgoff];
	if (vma->vm_end - vma->vm_start > mem->size)
		return -EINVAL;

	/* The donor mapped the region write-combined. */
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return remap_pfn_range(vma, vma->vm_start, mem->addr >> PAGE_SHIFT,
			       vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

static int msm_sharedmem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct uio_info *info;
	phys_addr_t phys;
	dma_addr_t dma;
	size_t size;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || !res->name)
		return dev_err_probe(dev, -ENODEV, "no named memory region\n");

	size = resource_size(res);
	if (!size)
		return dev_err_probe(dev, -EINVAL, "empty memory region\n");

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	phys = res->start;
	if (!phys) {
		/* The donor's dynamic case: allocate the region here. */
		if (!dmam_alloc_coherent(dev, size, &dma, GFP_KERNEL))
			return -ENOMEM;
		phys = dma;
	}

	info->name = res->name;
	info->version = "1.0";
	info->mmap = sharedmem_mmap;
	info->mem[0].addr = phys;
	info->mem[0].size = size;
	info->mem[0].memtype = UIO_MEM_PHYS;

	ret = devm_uio_register_device(dev, info);
	if (ret)
		return dev_err_probe(dev, ret, "uio register failed\n");

	dev_info(dev, "shared memory '%s' at %pa, %zu bytes\n",
		 info->name, &phys, size);

	return 0;
}

static const struct of_device_id msm_sharedmem_of_match[] = {
	{ .compatible = "qcom,sharedmem-uio" },
	{ }
};
MODULE_DEVICE_TABLE(of, msm_sharedmem_of_match);

static struct platform_driver msm_sharedmem_driver = {
	.probe = msm_sharedmem_probe,
	.driver = {
		.name = "msm_sharedmem",
		.of_match_table = msm_sharedmem_of_match,
	},
};
module_platform_driver(msm_sharedmem_driver);

MODULE_DESCRIPTION("Qualcomm shared memory UIO driver for the modem's remote storage");
MODULE_LICENSE("GPL");
