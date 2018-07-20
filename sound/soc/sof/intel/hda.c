// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Authors: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *	    Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 *	    Jeeja KP <jeeja.kp@intel.com>
 *	    Rander Wang <rander.wang@intel.com>
 *          Keyon Jie <yang.jie@linux.intel.com>
 */

/*
 * Hardware interface for generic Intel audio DSP HDA IP
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/pci.h>
#include <sound/hdaudio_ext.h>
#include <sound/sof.h>
#include <sound/pcm_params.h>
#include <linux/pm_runtime.h>

/* platform specific devices */
#include "shim.h"

#include <sound/hda_register.h>
#include <sound/hdaudio.h>
#include <sound/hda_i915.h>

#include "../../../pci/hda/hda_codec.h"
#include "../../codecs/hdac_hda.h"

#include "../sof-priv.h"
#include "../ops.h"
#include "hda.h"

/*
 * Register IO
 */

void hda_dsp_write(struct snd_sof_dev *sdev, void __iomem *addr, u32 value)
{
	writel(value, addr);
}

u32 hda_dsp_read(struct snd_sof_dev *sdev, void __iomem *addr)
{
	return readl(addr);
}

void hda_dsp_write64(struct snd_sof_dev *sdev, void __iomem *addr, u64 value)
{
	memcpy_toio(addr, &value, sizeof(value));
}

u64 hda_dsp_read64(struct snd_sof_dev *sdev, void __iomem *addr)
{
	u64 val;

	memcpy_fromio(&val, addr, sizeof(val));
	return val;
}

/*
 * Memory copy.
 */

void hda_dsp_block_write(struct snd_sof_dev *sdev, u32 offset, void *src,
			 size_t size)
{
	void __iomem *dest = sdev->bar[sdev->mmio_bar] + offset;
	u32 tmp = 0;
	int i, m, n;
	const u8 *src_byte = src;

	m = size / 4;
	n = size % 4;

	/* __iowrite32_copy use 32bit size values so divide by 4 */
	__iowrite32_copy(dest, src, m);

	if (n) {
		for (i = 0; i < n; i++)
			tmp |= (u32)*(src_byte + m * 4 + i) << (i * 8);
		__iowrite32_copy(dest + m * 4, &tmp, 1);
	}
}

void hda_dsp_block_read(struct snd_sof_dev *sdev, u32 offset, void *dest,
			size_t size)
{
	void __iomem *src = sdev->bar[sdev->mmio_bar] + offset;

	memcpy_fromio(dest, src, size);
}

/*
 * Debug
 */

struct hda_dsp_msg_code {
	u32 code;
	const char *msg;
};

static const struct hda_dsp_msg_code hda_dsp_rom_msg[] = {
	{HDA_DSP_ROM_FW_MANIFEST_LOADED, "status: manifest loaded"},
	{HDA_DSP_ROM_FW_FW_LOADED, "status: fw loaded"},
	{HDA_DSP_ROM_FW_ENTERED, "status: fw entered"},
	{HDA_DSP_ROM_CSE_ERROR, "error: cse error"},
	{HDA_DSP_ROM_CSE_WRONG_RESPONSE, "error: cse wrong response"},
	{HDA_DSP_ROM_IMR_TO_SMALL, "error: IMR too small"},
	{HDA_DSP_ROM_BASE_FW_NOT_FOUND, "error: base fw not found"},
	{HDA_DSP_ROM_CSE_VALIDATION_FAILED, "error: signature verification failed"},
	{HDA_DSP_ROM_IPC_FATAL_ERROR, "error: ipc fatal error"},
	{HDA_DSP_ROM_L2_CACHE_ERROR, "error: L2 cache error"},
	{HDA_DSP_ROM_LOAD_OFFSET_TO_SMALL, "error: load offset too small"},
	{HDA_DSP_ROM_API_PTR_INVALID, "error: API ptr invalid"},
	{HDA_DSP_ROM_BASEFW_INCOMPAT, "error: base fw incompatble"},
	{HDA_DSP_ROM_UNHANDLED_INTERRUPT, "error: unhandled interrupt"},
	{HDA_DSP_ROM_MEMORY_HOLE_ECC, "error: ECC memory hole"},
	{HDA_DSP_ROM_KERNEL_EXCEPTION, "error: kernel exception"},
	{HDA_DSP_ROM_USER_EXCEPTION, "error: user exception"},
	{HDA_DSP_ROM_UNEXPECTED_RESET, "error: unexpected reset"},
	{HDA_DSP_ROM_NULL_FW_ENTRY,	"error: null FW entry point"},
};

static void hda_dsp_get_status(struct snd_sof_dev *sdev)
{
	u32 status;
	int i;

	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_DSP_SRAM_REG_ROM_STATUS);

	for (i = 0; i < ARRAY_SIZE(hda_dsp_rom_msg); i++) {
		if (status == hda_dsp_rom_msg[i].code) {
			dev_err(sdev->dev, "%s - code %8.8x\n",
				hda_dsp_rom_msg[i].msg, status);
			return;
		}
	}

	/* not for us, must be generic sof message */
	dev_dbg(sdev->dev, "unknown ROM status value %8.8x\n", status);
}

static void hda_dsp_get_registers(struct snd_sof_dev *sdev,
				  struct sof_ipc_dsp_oops_xtensa *xoops,
				  u32 *stack, size_t stack_words)
{
	/* first read registers */
	hda_dsp_block_read(sdev, sdev->dsp_oops_offset, xoops, sizeof(*xoops));

	/* then get the stack */
	hda_dsp_block_read(sdev, sdev->dsp_oops_offset + sizeof(*xoops), stack,
			   stack_words * sizeof(u32));
}

void hda_dsp_dump(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	u32 stack[HDA_DSP_STACK_DUMP_SIZE];
	u32 status, panic;

	/* try APL specific status message types first */
	hda_dsp_get_status(sdev);

	/* now try generic SOF status messages */
	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_DSP_SRAM_REG_FW_STATUS);
	panic = snd_sof_dsp_read(sdev, HDA_DSP_BAR, HDA_DSP_SRAM_REG_FW_TRACEP);

	if (sdev->boot_complete) {
		hda_dsp_get_registers(sdev, &xoops, stack,
				      HDA_DSP_STACK_DUMP_SIZE);
		snd_sof_get_status(sdev, status, panic, &xoops, stack,
				   HDA_DSP_STACK_DUMP_SIZE);
	} else {
		dev_err(sdev->dev, "error: status = 0x%8.8x panic = 0x%8.8x\n",
			status, panic);
		hda_dsp_get_status(sdev);
	}
}

/*
 * IPC Mailbox IO
 */

void hda_dsp_mailbox_write(struct snd_sof_dev *sdev, u32 offset,
			   void *message, size_t bytes)
{
	void __iomem *dest = sdev->bar[sdev->mailbox_bar] + offset;

	memcpy_toio(dest, message, bytes);
}

void hda_dsp_mailbox_read(struct snd_sof_dev *sdev, u32 offset,
			  void *message, size_t bytes)
{
	void __iomem *src = sdev->bar[sdev->mailbox_bar] + offset;

	memcpy_fromio(message, src, bytes);
}

/*
 * Supported devices.
 */

static const struct sof_intel_dsp_desc chip_info[] = {
{
	/* Skylake */
	.id = 0x9d70,
	.cores_num = 2,
	.cores_mask = HDA_DSP_CORE_MASK(0) | HDA_DSP_CORE_MASK(1),
	.ipc_req = HDA_DSP_REG_HIPCI,
	.ipc_req_mask = HDA_DSP_REG_HIPCI_BUSY,
	.ipc_ack = HDA_DSP_REG_HIPCIE,
	.ipc_ack_mask = HDA_DSP_REG_HIPCIE_DONE,
	.ipc_ctl = HDA_DSP_REG_HIPCCTL,
	.ops = &sof_skl_ops,
},
{
	/* Kabylake */
	.id = 0x9d71,
	.cores_num = 2,
	.cores_mask = HDA_DSP_CORE_MASK(0) | HDA_DSP_CORE_MASK(1),
	.ipc_req = HDA_DSP_REG_HIPCI,
	.ipc_req_mask = HDA_DSP_REG_HIPCI_BUSY,
	.ipc_ack = HDA_DSP_REG_HIPCIE,
	.ipc_ack_mask = HDA_DSP_REG_HIPCIE_DONE,
	.ipc_ctl = HDA_DSP_REG_HIPCCTL,
	.ops = &sof_skl_ops,
},
{
	/* Apollolake - BXT-P */
	.id = 0x5a98,
	.cores_num = 2,
	.cores_mask = HDA_DSP_CORE_MASK(0) | HDA_DSP_CORE_MASK(1),
	.ipc_req = HDA_DSP_REG_HIPCI,
	.ipc_req_mask = HDA_DSP_REG_HIPCI_BUSY,
	.ipc_ack = HDA_DSP_REG_HIPCIE,
	.ipc_ack_mask = HDA_DSP_REG_HIPCIE_DONE,
	.ipc_ctl = HDA_DSP_REG_HIPCCTL,
	.ops = &sof_apl_ops,
},
{
	/* BXT-M */
	.id = 0x1a98,
	.cores_num = 2,
	.cores_mask = HDA_DSP_CORE_MASK(0) | HDA_DSP_CORE_MASK(1),
	.ipc_req = HDA_DSP_REG_HIPCI,
	.ipc_req_mask = HDA_DSP_REG_HIPCI_BUSY,
	.ipc_ack = HDA_DSP_REG_HIPCIE,
	.ipc_ack_mask = HDA_DSP_REG_HIPCIE_DONE,
	.ipc_ctl = HDA_DSP_REG_HIPCCTL,
	.ops = &sof_apl_ops,
},
{
	/* GeminiLake */
	.id = 0x3198,
	.cores_num = 2,
	.cores_mask = HDA_DSP_CORE_MASK(0) | HDA_DSP_CORE_MASK(1),
	.ipc_req = HDA_DSP_REG_HIPCI,
	.ipc_req_mask = HDA_DSP_REG_HIPCI_BUSY,
	.ipc_ack = HDA_DSP_REG_HIPCIE,
	.ipc_ack_mask = HDA_DSP_REG_HIPCIE_DONE,
	.ipc_ctl = HDA_DSP_REG_HIPCCTL,
	.ops = &sof_apl_ops,
},
{
	/* Cannonlake */
	.id = 0x9dc8,
	.cores_num = 4,
	.cores_mask = HDA_DSP_CORE_MASK(0) |
				HDA_DSP_CORE_MASK(1) |
				HDA_DSP_CORE_MASK(2) |
				HDA_DSP_CORE_MASK(3),
	.ipc_req = CNL_DSP_REG_HIPCIDR,
	.ipc_req_mask = CNL_DSP_REG_HIPCIDR_BUSY,
	.ipc_ack = CNL_DSP_REG_HIPCIDA,
	.ipc_ack_mask = CNL_DSP_REG_HIPCIDA_DONE,
	.ipc_ctl = CNL_DSP_REG_HIPCCTL,
	.ops = &sof_cnl_ops,
},
};

static const struct sof_intel_dsp_desc *get_chip_info(int pci_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(chip_info); i++) {
		if (chip_info[i].id == pci_id)
			return &chip_info[i];
	}

	return NULL;
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
static void sof_enable_miscbdcge(struct snd_sof_dev *sdev, bool enable)
{
	u32 val;

	val = enable ? PCI_CGCTL_MISCBDCGE_MASK : 0;

	snd_sof_pci_update_bits(sdev, PCI_CGCTL, PCI_CGCTL_MISCBDCGE_MASK, val);
}

/*
 * While performing reset, controller may not come back properly causing
 * issues, so recommendation is to set CGCTL.MISCBDCGE to 0 then do reset
 * (init chip) and then again set CGCTL.MISCBDCGE to 1
 */
static int sof_hda_init_chip(struct snd_sof_dev *sdev, bool full_reset)
{
	struct hdac_bus *bus = &sdev->hbus->core;
	int ret;

	sof_enable_miscbdcge(sdev, false);
	ret = snd_hdac_bus_init_chip(bus, full_reset);
	sof_enable_miscbdcge(sdev, true);

	return ret;
}

irqreturn_t sof_hda_stream_interrupt(int irq, void *context)
{
	struct hdac_bus *bus = context;
	u32 status;

	if (!pm_runtime_active(bus->dev))
		return IRQ_NONE;

	spin_lock(&bus->reg_lock);

	status = snd_hdac_chip_readl(bus, INTSTS);
	if (status == 0 || status == 0xffffffff) {
		spin_unlock(&bus->reg_lock);
		return IRQ_NONE;
	}

	/* clear rirb int */
	status = snd_hdac_chip_readb(bus, RIRBSTS);
	if (status & RIRB_INT_MASK) {
		if (status & RIRB_INT_RESPONSE)
			snd_hdac_bus_update_rirb(bus);
		snd_hdac_chip_writeb(bus, RIRBSTS, RIRB_INT_MASK);
	}

	spin_unlock(&bus->reg_lock);

	return snd_hdac_chip_readl(bus, INTSTS) ? IRQ_WAKE_THREAD : IRQ_HANDLED;
}

/* called from IRQ */
static void sof_stream_update(struct hdac_bus *bus, struct hdac_stream *hstr)
{
	snd_pcm_period_elapsed(hstr->substream);
}

irqreturn_t sof_hda_stream_threaded_handler(int irq, void *context)
{
	struct hdac_bus *bus = context;
	u32 status;

	status = snd_hdac_chip_readl(bus, INTSTS);

	snd_hdac_bus_handle_stream_irq(bus, status, sof_stream_update);

	return IRQ_HANDLED;
}

static int sof_hda_acquire_irq(struct hda_bus *hbus, int do_disconnect)
{
	int ret;

	/* register our IRQ */
	ret = request_threaded_irq(hbus->pci->irq, sof_hda_stream_interrupt,
				   sof_hda_stream_threaded_handler,
				    IRQF_SHARED, "AudioHDA", &hbus->core);

	if (ret) {
		dev_err(hbus->core.dev,
			"unable to grab IRQ %d, disabling device\n",
			hbus->pci->irq);
		return ret;
	}

	hbus->core.irq = hbus->pci->irq;
	pci_intx(hbus->pci, 1);

	return 0;
}

static int sof_hdac_first_init(struct snd_sof_dev *sdev)
{
	struct hda_bus *hbus = sdev->hbus;
	struct hdac_bus *bus = &hbus->core;
	struct pci_dev *pci = hbus->pci;
	int err;
	unsigned short gcap;

	bus->addr = pci_resource_start(pci, 0);
	bus->remap_addr = pci_ioremap_bar(pci, 0);
	if (bus->remap_addr == NULL) {
		dev_err(bus->dev, "ioremap error\n");
		return -ENXIO;
	}

	sof_hda_init_chip(sdev, true);

	snd_hdac_bus_parse_capabilities(bus);

	if (sof_hda_acquire_irq(hbus, 0) < 0)
		return -EBUSY;

	/* update BARs for sof, don't need parse them again */
	sdev->bar[HDA_DSP_HDA_BAR] = bus->remap_addr;
	sdev->bar[HDA_DSP_PP_BAR] = bus->ppcap;
	sdev->bar[HDA_DSP_SPIB_BAR] = bus->spbcap;
	sdev->bar[HDA_DSP_DRSM_BAR] = bus->drsmcap;

	return 0;
}

#define IDISP_INTEL_VENDOR_ID	0x80860000

/*
 * load the legacy codec driver
 */
static void sof_load_codec_module(struct hda_codec *codec)
{
#ifdef MODULE
	char modalias[MODULE_NAME_LEN];
	const char *mod = NULL;

	snd_hdac_codec_modalias(&codec->core, modalias, sizeof(modalias));
	mod = modalias;
	dev_dbg(&codec->core.dev, "loading %s codec module\n", mod);
	request_module(mod);
#endif
}

/*
 * Probe the given codec address
 */
static int sof_probe_hda_codec(struct hda_bus *hbus, int addr)
{
	unsigned int cmd = (addr << 28) | (AC_NODE_ROOT << 20) |
		(AC_VERB_PARAMETERS << 8) | AC_PAR_VENDOR_ID;
	unsigned int res = -1;
	struct hdac_hda_priv *hda_codec;
	struct hdac_device *hdev;
	int err;

	mutex_lock(&hbus->core.cmd_mutex);
	snd_hdac_bus_send_cmd(&hbus->core, cmd);
	snd_hdac_bus_get_response(&hbus->core, addr, &res);
	mutex_unlock(&hbus->core.cmd_mutex);
	if (res == -1)
		return -EIO;
	dev_dbg(hbus->core.dev, "codec #%d probed OK: %x\n", addr, res);

	hda_codec = devm_kzalloc(&hbus->pci->dev, sizeof(*hda_codec),
				 GFP_KERNEL);
	if (!hda_codec)
		return -ENOMEM;

	hda_codec->codec.bus = hbus;
	hdev = &hda_codec->codec.core;

	err = snd_hdac_ext_bus_device_init(&hbus->core, addr, hdev);
	if (err < 0)
		return err;

	/* use legacy bus only for HDA codecs, idisp uses ext bus */
	if ((res & 0xFFFF0000) != IDISP_INTEL_VENDOR_ID) {
		hdev->type = HDA_DEV_LEGACY;
		sof_load_codec_module(&hda_codec->codec);
	}
	return 0;
}

/* Codec initialization */
static void sof_hda_codec_create(struct snd_sof_dev *sdev)
{
	struct hda_bus *hbus = sdev->hbus;
	struct hdac_bus *bus = &hbus->core;
	int c, max_slots;

	max_slots = HDA_MAX_CODECS;

	/* First try to probe all given codec slots */
	for (c = 0; c < max_slots; c++) {
		if ((bus->codec_mask & (1 << c))) {
			if (sof_probe_hda_codec(hbus, c) < 0) {
				/*
				 * Some BIOSen give you wrong codec addresses
				 * that don't exist
				 */
				dev_warn(bus->dev,
					 "Codec #%d probe error; disabling it...\n", c);
				bus->codec_mask &= ~(1 << c);
				/*
				 * More badly, accessing to a non-existing
				 * codec often screws up the controller bus,
				 * and disturbs the further communications.
				 * Thus if an error occurs during probing,
				 * better to reset the controller bus to get
				 * back to the sanity state.
				 */
				snd_hdac_bus_stop_chip(bus);
				sof_hda_init_chip(sdev, true);
			}
		}
	}
}

static int sof_i915_init(struct hdac_bus *bus)
{
	int err;

	/*
	 * The HDMI codec is in GPU so we need to ensure that it is powered
	 * up and ready for probe
	 */
	err = snd_hdac_i915_init(bus);
	if (err < 0)
		return err;

	err = snd_hdac_display_power(bus, true);
	if (err < 0)
		dev_err(bus->dev, "Cannot turn on display power on i915\n");

	return err;
}

#else
static int sof_first_init(struct snd_sof_dev *sdev)
{
	struct pci_dev *pci = sdev->pci;

	/* HDA base */
	sdev->bar[HDA_DSP_HDA_BAR] = pci_ioremap_bar(pci, HDA_DSP_HDA_BAR);
	if (!sdev->bar[HDA_DSP_HDA_BAR]) {
		dev_err(&pci->dev, "error: ioremap error\n");
		/*
		 * FIXME: why do we return directly,
		 *  should we have a goto err here?
		 *  or should all these gotos be replaced
		 * by a return?
		 */
		return -ENXIO;
	}

	/* get controller capabilities */
	return hda_dsp_ctrl_get_caps(sdev);
}
#endif

/*
 * We don't need to do a full HDA codec probe as external HDA codec mode is
 * considered legacy and will not be supported under SOF. HDMI/DP HDA will
 * be supported in the DSP.
 */
int hda_dsp_probe(struct snd_sof_dev *sdev)
{
	struct pci_dev *pci = sdev->pci;
	struct sof_intel_hda_dev *hdev;
	struct sof_intel_hda_stream *stream;
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	struct hda_bus *hbus;
	struct hdac_bus *bus;
	struct hdac_ext_bus_ops *ext_ops;
	struct hdac_ext_link *hlink = NULL;
#endif
	const struct sof_intel_dsp_desc *chip;
	int i;
	int ret = 0;

	/* set DSP arch ops */
	sdev->arch_ops = &sof_xtensa_arch_ops;

	chip = get_chip_info(pci->device);
	if (!chip) {
		dev_err(sdev->dev, "no such device supported, chip id:%x\n",
			pci->device);
		return -EIO;
	}

	hdev = devm_kzalloc(&pci->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;
	sdev->hda = hdev;
	hdev->desc = chip;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	hbus = devm_kzalloc(&pci->dev, sizeof(*hbus), GFP_KERNEL);
	if (!hbus)
		return -ENOMEM;

	sdev->hbus = hbus;
	bus = &hbus->core;

	/* HDA bus init */
#if IS_ENABLED(CONFIG_SND_SOC_HDAC_HDA)
	ext_ops = snd_soc_hdac_hda_get_ops();
#endif
	snd_hdac_ext_bus_init(bus, &pci->dev, NULL, NULL, ext_ops);
	bus->use_posbuf = 1;
	bus->bdl_pos_adj = 0;

	mutex_init(&hbus->prepare_mutex);
	hbus->pci = pci;
	hbus->mixer_assigned = -1;
	hbus->modelname = "sofbus";
#endif

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	/* initialise hdac bus */
	ret = sof_hdac_first_init(sdev);
#else
	/* initialise resources for non-HDA */
	ret = sof_first_init(sdev);
#endif
	if (ret < 0)
		return ret;

	/* DSP base */
	sdev->bar[HDA_DSP_BAR] = pci_ioremap_bar(pci, HDA_DSP_BAR);
	if (!sdev->bar[HDA_DSP_BAR]) {
		dev_err(&pci->dev, "error: ioremap error\n");
		return -ENXIO;
	}
	sdev->mmio_bar = HDA_DSP_BAR;
	sdev->mailbox_bar = HDA_DSP_BAR;

	pci_set_master(pci);
	synchronize_irq(pci->irq);

	/* allow 64bit DMA address if supported by H/W */
	if (!dma_set_mask(&pci->dev, DMA_BIT_MASK(64))) {
		dma_set_coherent_mask(&pci->dev, DMA_BIT_MASK(64));
	} else {
		dma_set_mask(&pci->dev, DMA_BIT_MASK(32));
		dma_set_coherent_mask(&pci->dev, DMA_BIT_MASK(32));
	}

	/* init streams */
	ret = hda_dsp_stream_init(sdev);
	if (ret < 0) {
		dev_err(&pci->dev, "error: failed to init streams\n");
		/*
		 * not all errors are due to memory issues, but trying
		 * to free everything does not harm
		 */
		return ret;
	}

	/* initialize chip */
	snd_sof_pci_update_bits(sdev, PCI_TCSEL, 0x07, 0);

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	sof_hda_init_chip(sdev, true);

	device_disable_async_suspend(bus->dev);

	/* check if dsp is there */
	if (bus->ppcap)
		dev_dbg(&pci->dev, "PP capbility, will probe DSP later.\n");

	if (bus->mlcap)
		snd_hdac_ext_bus_get_ml_capabilities(bus);

	snd_hdac_bus_stop_chip(bus);


	/* probe i915 and HDA codecs */
	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		ret = sof_i915_init(bus);
		if (ret < 0)
			goto free_streams;
	}

	ret = sof_hda_init_chip(sdev, true);
	if (ret < 0) {
		dev_err(bus->dev, "Init chip failed with ret: %d\n", ret);
		if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI))
			snd_hdac_display_power(bus, false);
		goto free_streams;
	}

	/* codec detection */
	if (!bus->codec_mask)
		dev_info(bus->dev, "no hda codecs found!\n");

	/* create codec instances */
	sof_hda_codec_create(sdev);

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		ret = snd_hdac_display_power(bus, false);
		if (ret < 0) {
			dev_err(bus->dev, "Cannot turn off display power on i915\n");
			goto free_streams;
		}
	}

	/*
	 * we are done probing so decrement link counts
	 */
	list_for_each_entry(hlink, &bus->hlink_list, list)
		snd_hdac_ext_bus_link_put(bus, hlink);
#endif

	/* reset HDA controller */
	ret = hda_dsp_ctrl_link_reset(sdev);
	if (ret < 0) {
		dev_err(&pci->dev, "error: failed to reset HDA controller\n");
		goto free_streams;
	}

	/* clear stream status */
	for (i = 0 ; i < hdev->num_capture ; i++) {
		stream = &hdev->cstream[i];
		if (stream)
			snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
						stream->sd_offset +
						SOF_HDA_ADSP_REG_CL_SD_STS,
						SOF_HDA_CL_DMA_SD_INT_MASK,
						SOF_HDA_CL_DMA_SD_INT_MASK);
	}

	for (i = 0 ; i < hdev->num_playback ; i++) {
		stream = &hdev->pstream[i];
		if (stream)
			snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
						stream->sd_offset +
						SOF_HDA_ADSP_REG_CL_SD_STS,
						SOF_HDA_CL_DMA_SD_INT_MASK,
						SOF_HDA_CL_DMA_SD_INT_MASK);
	}

	/* clear WAKESTS */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_WAKESTS,
				SOF_HDA_WAKESTS_INT_MASK,
				SOF_HDA_WAKESTS_INT_MASK);

	/* clear interrupt status register */
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTSTS,
			  SOF_HDA_INT_CTRL_EN | SOF_HDA_INT_ALL_STREAM);

	/* enable CIE and GIE interrupts */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTCTL,
				SOF_HDA_INT_CTRL_EN | SOF_HDA_INT_GLOBAL_EN,
				SOF_HDA_INT_CTRL_EN | SOF_HDA_INT_GLOBAL_EN);

	sdev->ipc_irq = pci->irq;

	dev_dbg(sdev->dev, "using IPC IRQ %d\n", sdev->ipc_irq);
	ret = request_threaded_irq(sdev->ipc_irq, hda_dsp_ipc_irq_handler,
				   chip->ops->irq_thread, IRQF_SHARED,
				   "AudioDSP", sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register IPC IRQ %d\n",
			sdev->ipc_irq);
		goto free_streams;
	}

	/* re-enable CGCTL.MISCBDCGE after reset */
	snd_sof_pci_update_bits(sdev, PCI_CGCTL,
				PCI_CGCTL_MISCBDCGE_MASK,
				PCI_CGCTL_MISCBDCGE_MASK);

	device_disable_async_suspend(&pci->dev);

	/* enable DSP features */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				SOF_HDA_PPCTL_GPROCEN, SOF_HDA_PPCTL_GPROCEN);

	/* enable DSP IRQ */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				SOF_HDA_PPCTL_PIE, SOF_HDA_PPCTL_PIE);

	/* initialize waitq for code loading */
	init_waitqueue_head(&sdev->waitq);

	/* set default mailbox offset for FW ready message */
	sdev->dsp_box.offset = HDA_DSP_MBOX_UPLINK_OFFSET;

	return 0;

free_streams:
	hda_dsp_stream_free(sdev);

	return ret;
}

int hda_dsp_remove(struct snd_sof_dev *sdev)
{
	struct pci_dev *pci = sdev->pci;
	const struct sof_intel_dsp_desc *chip = sdev->hda->desc;

	/* disable DSP IRQ */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				SOF_HDA_PPCTL_PIE, 0);

	/* disable CIE and GIE interrupts */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTCTL,
				SOF_HDA_INT_CTRL_EN | SOF_HDA_INT_GLOBAL_EN, 0);

	/* disable cores */
	if (chip)
		hda_dsp_core_reset_power_down(sdev, chip->cores_mask);

	/* disable DSP */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				SOF_HDA_PPCTL_GPROCEN, 0);

	free_irq(sdev->ipc_irq, sdev);
	pci_free_irq_vectors(pci);

	hda_dsp_stream_free(sdev);
	return 0;
}

MODULE_LICENSE("Dual BSD/GPL");
