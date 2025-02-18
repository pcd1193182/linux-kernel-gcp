// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2019 Intel Corporation. All rights rsvd. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/workqueue.h>
#include <linux/aer.h>
#include <linux/fs.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/intel-svm.h>
#include <linux/iommu.h>
#include <uapi/linux/idxd.h>
#include <linux/dmaengine.h>
#include "../dmaengine.h"
#include "registers.h"
#include "idxd.h"

MODULE_VERSION(IDXD_DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Intel Corporation");

#define DRV_NAME "idxd"

bool support_enqcmd;

static struct ida idxd_idas[IDXD_TYPE_MAX];

static struct pci_device_id idxd_pci_tbl[] = {
	/* DSA ver 1.0 platforms */
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_DSA_SPR0) },

	/* IAX ver 1.0 platforms */
	{ PCI_VDEVICE(INTEL, PCI_DEVICE_ID_INTEL_IAX_SPR0) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, idxd_pci_tbl);

static char *idxd_name[] = {
	"dsa",
	"iax"
};

struct ida *idxd_ida(struct idxd_device *idxd)
{
	return &idxd_idas[idxd->type];
}

const char *idxd_get_dev_name(struct idxd_device *idxd)
{
	return idxd_name[idxd->type];
}

static int idxd_setup_interrupts(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	struct idxd_irq_entry *irq_entry;
	int i, msixcnt;
	int rc = 0;

	msixcnt = pci_msix_vec_count(pdev);
	if (msixcnt < 0) {
		dev_err(dev, "Not MSI-X interrupt capable.\n");
		return -ENOSPC;
	}

	rc = pci_alloc_irq_vectors(pdev, msixcnt, msixcnt, PCI_IRQ_MSIX);
	if (rc != msixcnt) {
		dev_err(dev, "Failed enabling %d MSIX entries: %d\n", msixcnt, rc);
		return -ENOSPC;
	}
	dev_dbg(dev, "Enabled %d msix vectors\n", msixcnt);

	/*
	 * We implement 1 completion list per MSI-X entry except for
	 * entry 0, which is for errors and others.
	 */
	idxd->irq_entries = kcalloc_node(msixcnt, sizeof(struct idxd_irq_entry),
					 GFP_KERNEL, dev_to_node(dev));
	if (!idxd->irq_entries) {
		rc = -ENOMEM;
		goto err_irq_entries;
	}

	for (i = 0; i < msixcnt; i++) {
		idxd->irq_entries[i].id = i;
		idxd->irq_entries[i].idxd = idxd;
		idxd->irq_entries[i].vector = pci_irq_vector(pdev, i);
		spin_lock_init(&idxd->irq_entries[i].list_lock);
	}

	idxd_msix_perm_setup(idxd);

	irq_entry = &idxd->irq_entries[0];
	rc = request_threaded_irq(irq_entry->vector, idxd_irq_handler, idxd_misc_thread,
				  0, "idxd-misc", irq_entry);
	if (rc < 0) {
		dev_err(dev, "Failed to allocate misc interrupt.\n");
		goto err_misc_irq;
	}

	dev_dbg(dev, "Allocated idxd-misc handler on msix vector %d\n", irq_entry->vector);

	/* first MSI-X entry is not for wq interrupts */
	idxd->num_wq_irqs = msixcnt - 1;

	for (i = 1; i < msixcnt; i++) {
		irq_entry = &idxd->irq_entries[i];

		init_llist_head(&idxd->irq_entries[i].pending_llist);
		INIT_LIST_HEAD(&idxd->irq_entries[i].work_list);
		rc = request_threaded_irq(irq_entry->vector, idxd_irq_handler,
					  idxd_wq_thread, 0, "idxd-portal", irq_entry);
		if (rc < 0) {
			dev_err(dev, "Failed to allocate irq %d.\n", irq_entry->vector);
			goto err_wq_irqs;
		}
		dev_dbg(dev, "Allocated idxd-msix %d for vector %d\n", i, irq_entry->vector);
	}

	idxd_unmask_error_interrupts(idxd);
	return 0;

 err_wq_irqs:
	while (--i >= 0) {
		irq_entry = &idxd->irq_entries[i];
		free_irq(irq_entry->vector, irq_entry);
	}
 err_misc_irq:
	/* Disable error interrupt generation */
	idxd_mask_error_interrupts(idxd);
	idxd_msix_perm_clear(idxd);
 err_irq_entries:
	pci_free_irq_vectors(pdev);
	dev_err(dev, "No usable interrupts\n");
	return rc;
}

static int idxd_setup_wqs(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	struct idxd_wq *wq;
	int i, rc;

	idxd->wqs = kcalloc_node(idxd->max_wqs, sizeof(struct idxd_wq *),
				 GFP_KERNEL, dev_to_node(dev));
	if (!idxd->wqs)
		return -ENOMEM;

	for (i = 0; i < idxd->max_wqs; i++) {
		wq = kzalloc_node(sizeof(*wq), GFP_KERNEL, dev_to_node(dev));
		if (!wq) {
			rc = -ENOMEM;
			goto err;
		}

		wq->id = i;
		wq->idxd = idxd;
		device_initialize(&wq->conf_dev);
		wq->conf_dev.parent = &idxd->conf_dev;
		wq->conf_dev.bus = idxd_get_bus_type(idxd);
		wq->conf_dev.type = &idxd_wq_device_type;
		rc = dev_set_name(&wq->conf_dev, "wq%d.%d", idxd->id, wq->id);
		if (rc < 0) {
			put_device(&wq->conf_dev);
			goto err;
		}

		mutex_init(&wq->wq_lock);
		init_waitqueue_head(&wq->err_queue);
		wq->max_xfer_bytes = idxd->max_xfer_bytes;
		wq->max_batch_size = idxd->max_batch_size;
		wq->wqcfg = kzalloc_node(idxd->wqcfg_size, GFP_KERNEL, dev_to_node(dev));
		if (!wq->wqcfg) {
			put_device(&wq->conf_dev);
			rc = -ENOMEM;
			goto err;
		}
		idxd->wqs[i] = wq;
	}

	return 0;

 err:
	while (--i >= 0)
		put_device(&idxd->wqs[i]->conf_dev);
	return rc;
}

static int idxd_setup_engines(struct idxd_device *idxd)
{
	struct idxd_engine *engine;
	struct device *dev = &idxd->pdev->dev;
	int i, rc;

	idxd->engines = kcalloc_node(idxd->max_engines, sizeof(struct idxd_engine *),
				     GFP_KERNEL, dev_to_node(dev));
	if (!idxd->engines)
		return -ENOMEM;

	for (i = 0; i < idxd->max_engines; i++) {
		engine = kzalloc_node(sizeof(*engine), GFP_KERNEL, dev_to_node(dev));
		if (!engine) {
			rc = -ENOMEM;
			goto err;
		}

		engine->id = i;
		engine->idxd = idxd;
		device_initialize(&engine->conf_dev);
		engine->conf_dev.parent = &idxd->conf_dev;
		engine->conf_dev.bus = &dsa_bus_type;
		engine->conf_dev.type = &idxd_engine_device_type;
		rc = dev_set_name(&engine->conf_dev, "engine%d.%d", idxd->id, engine->id);
		if (rc < 0) {
			put_device(&engine->conf_dev);
			goto err;
		}

		idxd->engines[i] = engine;
	}

	return 0;

 err:
	while (--i >= 0)
		put_device(&idxd->engines[i]->conf_dev);
	return rc;
}

static int idxd_setup_groups(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	struct idxd_group *group;
	int i, rc;

	idxd->groups = kcalloc_node(idxd->max_groups, sizeof(struct idxd_group *),
				    GFP_KERNEL, dev_to_node(dev));
	if (!idxd->groups)
		return -ENOMEM;

	for (i = 0; i < idxd->max_groups; i++) {
		group = kzalloc_node(sizeof(*group), GFP_KERNEL, dev_to_node(dev));
		if (!group) {
			rc = -ENOMEM;
			goto err;
		}

		group->id = i;
		group->idxd = idxd;
		device_initialize(&group->conf_dev);
		group->conf_dev.parent = &idxd->conf_dev;
		group->conf_dev.bus = idxd_get_bus_type(idxd);
		group->conf_dev.type = &idxd_group_device_type;
		rc = dev_set_name(&group->conf_dev, "group%d.%d", idxd->id, group->id);
		if (rc < 0) {
			put_device(&group->conf_dev);
			goto err;
		}

		idxd->groups[i] = group;
		group->tc_a = -1;
		group->tc_b = -1;
	}

	return 0;

 err:
	while (--i >= 0)
		put_device(&idxd->groups[i]->conf_dev);
	return rc;
}

static int idxd_setup_internals(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	int rc, i;

	init_waitqueue_head(&idxd->cmd_waitq);

	rc = idxd_setup_wqs(idxd);
	if (rc < 0)
		return rc;

	rc = idxd_setup_engines(idxd);
	if (rc < 0)
		goto err_engine;

	rc = idxd_setup_groups(idxd);
	if (rc < 0)
		goto err_group;

	idxd->wq = create_workqueue(dev_name(dev));
	if (!idxd->wq) {
		rc = -ENOMEM;
		goto err_wkq_create;
	}

	return 0;

 err_wkq_create:
	for (i = 0; i < idxd->max_groups; i++)
		put_device(&idxd->groups[i]->conf_dev);
 err_group:
	for (i = 0; i < idxd->max_engines; i++)
		put_device(&idxd->engines[i]->conf_dev);
 err_engine:
	for (i = 0; i < idxd->max_wqs; i++)
		put_device(&idxd->wqs[i]->conf_dev);
	return rc;
}

static void idxd_read_table_offsets(struct idxd_device *idxd)
{
	union offsets_reg offsets;
	struct device *dev = &idxd->pdev->dev;

	offsets.bits[0] = ioread64(idxd->reg_base + IDXD_TABLE_OFFSET);
	offsets.bits[1] = ioread64(idxd->reg_base + IDXD_TABLE_OFFSET + sizeof(u64));
	idxd->grpcfg_offset = offsets.grpcfg * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD Group Config Offset: %#x\n", idxd->grpcfg_offset);
	idxd->wqcfg_offset = offsets.wqcfg * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD Work Queue Config Offset: %#x\n", idxd->wqcfg_offset);
	idxd->msix_perm_offset = offsets.msix_perm * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD MSIX Permission Offset: %#x\n", idxd->msix_perm_offset);
	idxd->perfmon_offset = offsets.perfmon * IDXD_TABLE_MULT;
	dev_dbg(dev, "IDXD Perfmon Offset: %#x\n", idxd->perfmon_offset);
}

static void idxd_read_caps(struct idxd_device *idxd)
{
	struct device *dev = &idxd->pdev->dev;
	int i;

	/* reading generic capabilities */
	idxd->hw.gen_cap.bits = ioread64(idxd->reg_base + IDXD_GENCAP_OFFSET);
	dev_dbg(dev, "gen_cap: %#llx\n", idxd->hw.gen_cap.bits);
	idxd->max_xfer_bytes = 1ULL << idxd->hw.gen_cap.max_xfer_shift;
	dev_dbg(dev, "max xfer size: %llu bytes\n", idxd->max_xfer_bytes);
	idxd->max_batch_size = 1U << idxd->hw.gen_cap.max_batch_shift;
	dev_dbg(dev, "max batch size: %u\n", idxd->max_batch_size);
	if (idxd->hw.gen_cap.config_en)
		set_bit(IDXD_FLAG_CONFIGURABLE, &idxd->flags);

	/* reading group capabilities */
	idxd->hw.group_cap.bits =
		ioread64(idxd->reg_base + IDXD_GRPCAP_OFFSET);
	dev_dbg(dev, "group_cap: %#llx\n", idxd->hw.group_cap.bits);
	idxd->max_groups = idxd->hw.group_cap.num_groups;
	dev_dbg(dev, "max groups: %u\n", idxd->max_groups);
	idxd->max_tokens = idxd->hw.group_cap.total_tokens;
	dev_dbg(dev, "max tokens: %u\n", idxd->max_tokens);
	idxd->nr_tokens = idxd->max_tokens;

	/* read engine capabilities */
	idxd->hw.engine_cap.bits =
		ioread64(idxd->reg_base + IDXD_ENGCAP_OFFSET);
	dev_dbg(dev, "engine_cap: %#llx\n", idxd->hw.engine_cap.bits);
	idxd->max_engines = idxd->hw.engine_cap.num_engines;
	dev_dbg(dev, "max engines: %u\n", idxd->max_engines);

	/* read workqueue capabilities */
	idxd->hw.wq_cap.bits = ioread64(idxd->reg_base + IDXD_WQCAP_OFFSET);
	dev_dbg(dev, "wq_cap: %#llx\n", idxd->hw.wq_cap.bits);
	idxd->max_wq_size = idxd->hw.wq_cap.total_wq_size;
	dev_dbg(dev, "total workqueue size: %u\n", idxd->max_wq_size);
	idxd->max_wqs = idxd->hw.wq_cap.num_wqs;
	dev_dbg(dev, "max workqueues: %u\n", idxd->max_wqs);
	idxd->wqcfg_size = 1 << (idxd->hw.wq_cap.wqcfg_size + IDXD_WQCFG_MIN);
	dev_dbg(dev, "wqcfg size: %u\n", idxd->wqcfg_size);

	/* reading operation capabilities */
	for (i = 0; i < 4; i++) {
		idxd->hw.opcap.bits[i] = ioread64(idxd->reg_base +
				IDXD_OPCAP_OFFSET + i * sizeof(u64));
		dev_dbg(dev, "opcap[%d]: %#llx\n", i, idxd->hw.opcap.bits[i]);
	}
}

static inline void idxd_set_type(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;

	if (pdev->device == PCI_DEVICE_ID_INTEL_DSA_SPR0)
		idxd->type = IDXD_TYPE_DSA;
	else if (pdev->device == PCI_DEVICE_ID_INTEL_IAX_SPR0)
		idxd->type = IDXD_TYPE_IAX;
	else
		idxd->type = IDXD_TYPE_UNKNOWN;
}

static struct idxd_device *idxd_alloc(struct pci_dev *pdev)
{
	struct device *dev = &pdev->dev;
	struct idxd_device *idxd;
	int rc;

	idxd = kzalloc_node(sizeof(*idxd), GFP_KERNEL, dev_to_node(dev));
	if (!idxd)
		return NULL;

	idxd->pdev = pdev;
	idxd_set_type(idxd);
	idxd->id = ida_alloc(idxd_ida(idxd), GFP_KERNEL);
	if (idxd->id < 0)
		return NULL;

	device_initialize(&idxd->conf_dev);
	idxd->conf_dev.parent = dev;
	idxd->conf_dev.bus = idxd_get_bus_type(idxd);
	idxd->conf_dev.type = idxd_get_device_type(idxd);
	rc = dev_set_name(&idxd->conf_dev, "%s%d", idxd_get_dev_name(idxd), idxd->id);
	if (rc < 0) {
		put_device(&idxd->conf_dev);
		return NULL;
	}

	spin_lock_init(&idxd->dev_lock);

	return idxd;
}

static int idxd_enable_system_pasid(struct idxd_device *idxd)
{
	int flags;
	unsigned int pasid;
	struct iommu_sva *sva;

	flags = SVM_FLAG_SUPERVISOR_MODE;

	sva = iommu_sva_bind_device(&idxd->pdev->dev, NULL, &flags);
	if (IS_ERR(sva)) {
		dev_warn(&idxd->pdev->dev,
			 "iommu sva bind failed: %ld\n", PTR_ERR(sva));
		return PTR_ERR(sva);
	}

	pasid = iommu_sva_get_pasid(sva);
	if (pasid == IOMMU_PASID_INVALID) {
		iommu_sva_unbind_device(sva);
		return -ENODEV;
	}

	idxd->sva = sva;
	idxd->pasid = pasid;
	dev_dbg(&idxd->pdev->dev, "system pasid: %u\n", pasid);
	return 0;
}

static void idxd_disable_system_pasid(struct idxd_device *idxd)
{

	iommu_sva_unbind_device(idxd->sva);
	idxd->sva = NULL;
}

static int idxd_probe(struct idxd_device *idxd)
{
	struct pci_dev *pdev = idxd->pdev;
	struct device *dev = &pdev->dev;
	int rc;

	dev_dbg(dev, "%s entered and resetting device\n", __func__);
	rc = idxd_device_init_reset(idxd);
	if (rc < 0)
		return rc;

	dev_dbg(dev, "IDXD reset complete\n");

	if (IS_ENABLED(CONFIG_INTEL_IDXD_SVM)) {
		rc = idxd_enable_system_pasid(idxd);
		if (rc < 0)
			dev_warn(dev, "Failed to enable PASID. No SVA support: %d\n", rc);
		else
			set_bit(IDXD_FLAG_PASID_ENABLED, &idxd->flags);
	}

	idxd_read_caps(idxd);
	idxd_read_table_offsets(idxd);

	rc = idxd_setup_internals(idxd);
	if (rc)
		goto err;

	rc = idxd_setup_interrupts(idxd);
	if (rc)
		goto err;

	dev_dbg(dev, "IDXD interrupt setup complete.\n");

	idxd->major = idxd_cdev_get_major(idxd);

	dev_dbg(dev, "IDXD device %d probed successfully\n", idxd->id);
	return 0;

 err:
	if (device_pasid_enabled(idxd))
		idxd_disable_system_pasid(idxd);
	return rc;
}

static void idxd_type_init(struct idxd_device *idxd)
{
	if (idxd->type == IDXD_TYPE_DSA)
		idxd->compl_size = sizeof(struct dsa_completion_record);
	else if (idxd->type == IDXD_TYPE_IAX)
		idxd->compl_size = sizeof(struct iax_completion_record);
}

static int idxd_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct idxd_device *idxd;
	int rc;

	rc = pci_enable_device(pdev);
	if (rc)
		return rc;

	dev_dbg(dev, "Alloc IDXD context\n");
	idxd = idxd_alloc(pdev);
	if (!idxd) {
		rc = -ENOMEM;
		goto err_idxd_alloc;
	}

	dev_dbg(dev, "Mapping BARs\n");
	idxd->reg_base = pci_iomap(pdev, IDXD_MMIO_BAR, 0);
	if (!idxd->reg_base) {
		rc = -ENOMEM;
		goto err_iomap;
	}

	dev_dbg(dev, "Set DMA masks\n");
	rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (rc)
		rc = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (rc)
		goto err;

	rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (rc)
		rc = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(32));
	if (rc)
		goto err;


	idxd_type_init(idxd);

	dev_dbg(dev, "Set PCI master\n");
	pci_set_master(pdev);
	pci_set_drvdata(pdev, idxd);

	idxd->hw.version = ioread32(idxd->reg_base + IDXD_VER_OFFSET);
	rc = idxd_probe(idxd);
	if (rc) {
		dev_err(dev, "Intel(R) IDXD DMA Engine init failed\n");
		goto err;
	}

	rc = idxd_register_devices(idxd);
	if (rc) {
		dev_err(dev, "IDXD sysfs setup failed\n");
		goto err;
	}

	idxd->state = IDXD_DEV_CONF_READY;

	dev_info(&pdev->dev, "Intel(R) Accelerator Device (v%x)\n",
		 idxd->hw.version);

	return 0;

 err:
	pci_iounmap(pdev, idxd->reg_base);
 err_iomap:
	put_device(&idxd->conf_dev);
 err_idxd_alloc:
	pci_disable_device(pdev);
	return rc;
}

static void idxd_flush_pending_llist(struct idxd_irq_entry *ie)
{
	struct idxd_desc *desc, *itr;
	struct llist_node *head;

	head = llist_del_all(&ie->pending_llist);
	if (!head)
		return;

	llist_for_each_entry_safe(desc, itr, head, llnode) {
		idxd_dma_complete_txd(desc, IDXD_COMPLETE_ABORT);
		idxd_free_desc(desc->wq, desc);
	}
}

static void idxd_flush_work_list(struct idxd_irq_entry *ie)
{
	struct idxd_desc *desc, *iter;

	list_for_each_entry_safe(desc, iter, &ie->work_list, list) {
		list_del(&desc->list);
		idxd_dma_complete_txd(desc, IDXD_COMPLETE_ABORT);
		idxd_free_desc(desc->wq, desc);
	}
}

static void idxd_shutdown(struct pci_dev *pdev)
{
	struct idxd_device *idxd = pci_get_drvdata(pdev);
	int rc, i;
	struct idxd_irq_entry *irq_entry;
	int msixcnt = pci_msix_vec_count(pdev);

	rc = idxd_device_disable(idxd);
	if (rc)
		dev_err(&pdev->dev, "Disabling device failed\n");

	dev_dbg(&pdev->dev, "%s called\n", __func__);
	idxd_mask_msix_vectors(idxd);
	idxd_mask_error_interrupts(idxd);

	for (i = 0; i < msixcnt; i++) {
		irq_entry = &idxd->irq_entries[i];
		synchronize_irq(irq_entry->vector);
		free_irq(irq_entry->vector, irq_entry);
		if (i == 0)
			continue;
		idxd_flush_pending_llist(irq_entry);
		idxd_flush_work_list(irq_entry);
	}

	idxd_msix_perm_clear(idxd);
	pci_free_irq_vectors(pdev);
	pci_iounmap(pdev, idxd->reg_base);
	pci_disable_device(pdev);
	destroy_workqueue(idxd->wq);
}

static void idxd_remove(struct pci_dev *pdev)
{
	struct idxd_device *idxd = pci_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "%s called\n", __func__);
	idxd_shutdown(pdev);
	if (device_pasid_enabled(idxd))
		idxd_disable_system_pasid(idxd);
	idxd_unregister_devices(idxd);
}

static struct pci_driver idxd_pci_driver = {
	.name		= DRV_NAME,
	.id_table	= idxd_pci_tbl,
	.probe		= idxd_pci_probe,
	.remove		= idxd_remove,
	.shutdown	= idxd_shutdown,
};

static int __init idxd_init_module(void)
{
	int err, i;

	/*
	 * If the CPU does not support MOVDIR64B or ENQCMDS, there's no point in
	 * enumerating the device. We can not utilize it.
	 */
	if (!cpu_feature_enabled(X86_FEATURE_MOVDIR64B)) {
		pr_warn("idxd driver failed to load without MOVDIR64B.\n");
		return -ENODEV;
	}

	if (!cpu_feature_enabled(X86_FEATURE_ENQCMD))
		pr_warn("Platform does not have ENQCMD(S) support.\n");
	else
		support_enqcmd = true;

	for (i = 0; i < IDXD_TYPE_MAX; i++)
		ida_init(&idxd_idas[i]);

	err = idxd_register_bus_type();
	if (err < 0)
		return err;

	err = idxd_register_driver();
	if (err < 0)
		goto err_idxd_driver_register;

	err = idxd_cdev_register();
	if (err)
		goto err_cdev_register;

	err = pci_register_driver(&idxd_pci_driver);
	if (err)
		goto err_pci_register;

	return 0;

err_pci_register:
	idxd_cdev_remove();
err_cdev_register:
	idxd_unregister_driver();
err_idxd_driver_register:
	idxd_unregister_bus_type();
	return err;
}
module_init(idxd_init_module);

static void __exit idxd_exit_module(void)
{
	idxd_unregister_driver();
	pci_unregister_driver(&idxd_pci_driver);
	idxd_cdev_remove();
	idxd_unregister_bus_type();
}
module_exit(idxd_exit_module);
