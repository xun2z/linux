// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Authors: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//	    Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//	    Rander Wang <rander.wang@intel.com>
//          Keyon Jie <yang.jie@linux.intel.com>
//

/*
 * Hardware interface for generic Intel audio DSP HDA IP
 */

#include <linux/module.h>
#include <sound/hdaudio_ext.h>
#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include "../ops.h"
#include "hda.h"
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA_AUDIO_CODEC)
#include "../../codecs/hdac_hda.h"
#endif

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
#include <sound/soc-acpi-intel-match.h>
#endif

/* platform specific devices */
#include "shim.h"

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

static void hda_dsp_get_status_skl(struct snd_sof_dev *sdev)
{
	u32 status;
	int i;

	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_ADSP_FW_STATUS_SKL);

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
				  struct sof_ipc_panic_info *panic_info,
				  u32 *stack, size_t stack_words)
{
	/* first read registers */
	sof_block_read(sdev, sdev->mmio_bar, sdev->dsp_oops_offset, xoops,
		       sizeof(*xoops));

	/* then get panic info */
	sof_block_read(sdev, sdev->mmio_bar, sdev->dsp_oops_offset +
		       sizeof(*xoops), panic_info, sizeof(*panic_info));

	/* then get the stack */
	sof_block_read(sdev, sdev->mmio_bar, sdev->dsp_oops_offset +
		       sizeof(*xoops) + sizeof(*panic_info), stack,
		       stack_words * sizeof(u32));
}

void hda_dsp_dump_skl(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	struct sof_ipc_panic_info panic_info;
	u32 stack[HDA_DSP_STACK_DUMP_SIZE];
	u32 status, panic;

	/* try APL specific status message types first */
	hda_dsp_get_status_skl(sdev);

	/* now try generic SOF status messages */
	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_ADSP_ERROR_CODE_SKL);

	/*TODO: Check: there is no define in spec, but it is used in the code*/
	panic = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				 HDA_ADSP_ERROR_CODE_SKL + 0x4);

	if (sdev->boot_complete) {
		hda_dsp_get_registers(sdev, &xoops, &panic_info, stack,
				      HDA_DSP_STACK_DUMP_SIZE);
		snd_sof_get_status(sdev, status, panic, &xoops, &panic_info,
				   stack, HDA_DSP_STACK_DUMP_SIZE);
	} else {
		dev_err(sdev->dev, "error: status = 0x%8.8x panic = 0x%8.8x\n",
			status, panic);
		hda_dsp_get_status_skl(sdev);
	}
}

void hda_dsp_dump(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	struct sof_ipc_panic_info panic_info;
	u32 stack[HDA_DSP_STACK_DUMP_SIZE];
	u32 status, panic;

	/* try APL specific status message types first */
	hda_dsp_get_status(sdev);

	/* now try generic SOF status messages */
	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_DSP_SRAM_REG_FW_STATUS);
	panic = snd_sof_dsp_read(sdev, HDA_DSP_BAR, HDA_DSP_SRAM_REG_FW_TRACEP);

	if (sdev->boot_complete) {
		hda_dsp_get_registers(sdev, &xoops, &panic_info, stack,
				      HDA_DSP_STACK_DUMP_SIZE);
		snd_sof_get_status(sdev, status, panic, &xoops, &panic_info,
				   stack, HDA_DSP_STACK_DUMP_SIZE);
	} else {
		dev_err(sdev->dev, "error: status = 0x%8.8x panic = 0x%8.8x\n",
			status, panic);
		hda_dsp_get_status(sdev);
	}
}

static int hda_init(struct snd_sof_dev *sdev)
{
	struct hda_bus *hbus;
	struct hdac_bus *bus;
	struct hdac_ext_bus_ops *ext_ops = NULL;
	struct pci_dev *pci = to_pci_dev(sdev->dev);
	int ret;

	hbus = sof_to_hbus(sdev);
	bus = sof_to_bus(sdev);

	/* HDA bus init */
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA_AUDIO_CODEC)
	ext_ops = snd_soc_hdac_hda_get_ops();
#endif
	sof_hda_bus_init(bus, &pci->dev, ext_ops);
	bus->use_posbuf = 1;
	bus->bdl_pos_adj = 0;

	mutex_init(&hbus->prepare_mutex);
	hbus->pci = pci;
	hbus->mixer_assigned = -1;
	hbus->modelname = "sofbus";

	/* initialise hdac bus */
	bus->addr = pci_resource_start(pci, 0);
	bus->remap_addr = pci_ioremap_bar(pci, 0);
	if (!bus->remap_addr) {
		dev_err(bus->dev, "error: ioremap error\n");
		return -ENXIO;
	}

	/* HDA base */
	sdev->bar[HDA_DSP_HDA_BAR] = bus->remap_addr;

	/* get controller capabilities */
	ret = hda_dsp_ctrl_get_caps(sdev);
	if (ret < 0)
		dev_err(sdev->dev, "error: get caps error\n");

	return ret;
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)

static const char *fixup_tplg_name(struct snd_sof_dev *sdev,
				   const char *sof_tplg_filename)
{
	const char *tplg_filename = NULL;
	char *filename;
	char *split_ext;

	filename = devm_kstrdup(sdev->dev, sof_tplg_filename, GFP_KERNEL);
	if (!filename)
		return NULL;

	/* this assumes a .tplg extension */
	split_ext = strsep(&filename, ".");
	if (split_ext) {
		tplg_filename = devm_kasprintf(sdev->dev, GFP_KERNEL,
					       "%s-idisp.tplg", split_ext);
		if (!tplg_filename)
			return NULL;
	}
	return tplg_filename;
}

static int hda_init_caps(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct hdac_ext_link *hlink = NULL;
	struct snd_soc_acpi_mach_params *mach_params;
	struct snd_soc_acpi_mach *hda_mach = NULL;
	struct snd_sof_pdata *pdata = sdev->pdata;
	struct snd_soc_acpi_mach *mach;
	const char *tplg_filename;
	int codec_num = 0;
	int ret = 0;
	int i;

	device_disable_async_suspend(bus->dev);

	/* check if dsp is there */
	if (bus->ppcap)
		dev_dbg(sdev->dev, "PP capability, will probe DSP later.\n");

	if (bus->mlcap)
		snd_hdac_ext_bus_get_ml_capabilities(bus);

	/* init i915 and HDMI codecs */
	ret = hda_codec_i915_init(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: no HDMI audio devices found\n");
		return ret;
	}

	ret = hda_dsp_ctrl_init_chip(sdev, true);
	if (ret < 0) {
		dev_err(bus->dev, "error: init chip failed with ret: %d\n", ret);
		goto out;
	}

	/* codec detection */
	if (!bus->codec_mask) {
		dev_info(bus->dev, "no hda codecs found!\n");
	} else {
		dev_info(bus->dev, "hda codecs found, mask %lx\n",
			 bus->codec_mask);

		for (i = 0; i < HDA_MAX_CODECS; i++) {
			if (bus->codec_mask & (1 << i))
				codec_num++;
		}

		/*
		 * If no machine driver is found, then:
		 *
		 * hda machine driver is used if :
		 * 1. there is one HDMI codec and one external HDAudio codec
		 * 2. only HDMI codec
		 */
		if (!pdata->machine && codec_num <= 2 &&
		    HDA_IDISP_CODEC(bus->codec_mask)) {
			hda_mach = snd_soc_acpi_intel_hda_machines;
			pdata->machine = hda_mach;

			/* topology: use the info from hda_machines */
			pdata->tplg_filename =
				hda_mach->sof_tplg_filename;

			/* firmware: pick the first in machine list */
			mach = pdata->desc->machines;
			pdata->fw_filename = mach->sof_fw_filename;

			dev_info(bus->dev, "using HDA machine driver %s now\n",
				 hda_mach->drv_name);

			/* fixup topology file for HDMI only platforms */
			if (codec_num == 1) {
				/* use local variable for readability */
				tplg_filename = pdata->tplg_filename;
				tplg_filename = fixup_tplg_name(sdev, tplg_filename);
				if (!tplg_filename)
					goto out;
				pdata->tplg_filename = tplg_filename;
			}
		}
	}

	/* used by hda machine driver to create dai links */
	if (pdata->machine) {
		mach_params = (struct snd_soc_acpi_mach_params *)
			&pdata->machine->mach_params;
		mach_params->codec_mask = bus->codec_mask;
		mach_params->platform = dev_name(sdev->dev);
	}

	/* create codec instances */
	hda_codec_probe_bus(sdev);

	hda_codec_i915_put(sdev);

	/*
	 * we are done probing so decrement link counts
	 */
	list_for_each_entry(hlink, &bus->hlink_list, list)
		snd_hdac_ext_bus_link_put(bus, hlink);

	return 0;

out:
	hda_codec_i915_exit(sdev);
	return ret;
}

#else

static int hda_init_caps(struct snd_sof_dev *sdev)
{
	/*
	 * set CGCTL.MISCBDCGE to 0 during reset and set back to 1
	 * when reset finished.
	 * TODO: maybe no need for init_caps?
	 */
	hda_dsp_ctrl_misc_clock_gating(sdev, 0);

	/* clear WAKESTS */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_WAKESTS,
				SOF_HDA_WAKESTS_INT_MASK,
				SOF_HDA_WAKESTS_INT_MASK);

	return 0;
}

#endif

static const struct sof_intel_dsp_desc
	*get_chip_info(struct snd_sof_pdata *pdata)
{
	const struct sof_dev_desc *desc = pdata->desc;
	const struct sof_intel_dsp_desc *chip_info;

	chip_info = (struct sof_intel_dsp_desc *)desc->chip_info;

	return chip_info;
}

int hda_dsp_probe(struct snd_sof_dev *sdev)
{
	struct pci_dev *pci = to_pci_dev(sdev->dev);
	struct sof_intel_hda_dev *hdev;
	struct hdac_bus *bus;
	struct hdac_stream *stream;
	const struct sof_intel_dsp_desc *chip;
	int sd_offset, ret = 0;

	/*
	 * detect DSP by checking class/subclass/prog-id information
	 * class=04 subclass 03 prog-if 00: no DSP, legacy driver is required
	 * class=04 subclass 01 prog-if 00: DSP is present
	 *   (and may be required e.g. for DMIC or SSP support)
	 * class=04 subclass 03 prog-if 80: either of DSP or legacy mode works
	 */
	if (pci->class == 0x040300) {
		dev_err(sdev->dev, "error: the DSP is not enabled on this platform, aborting probe\n");
		return -ENODEV;
	} else if (pci->class != 0x040100 && pci->class != 0x040380) {
		dev_err(sdev->dev, "error: unknown PCI class/subclass/prog-if 0x%06x found, aborting probe\n", pci->class);
		return -ENODEV;
	}
	dev_info(sdev->dev, "DSP detected with PCI class/subclass/prog-if 0x%06x\n", pci->class);

	chip = get_chip_info(sdev->pdata);
	if (!chip) {
		dev_err(sdev->dev, "error: no such device supported, chip id:%x\n",
			pci->device);
		ret = -EIO;
		goto err;
	}

	hdev = devm_kzalloc(sdev->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;
	sdev->pdata->hw_pdata = hdev;
	hdev->desc = chip;

	hdev->dmic_dev = platform_device_register_data(sdev->dev, "dmic-codec",
						       PLATFORM_DEVID_NONE,
						       NULL, 0);
	if (IS_ERR(hdev->dmic_dev)) {
		dev_err(sdev->dev, "error: failed to create DMIC device\n");
		return PTR_ERR(hdev->dmic_dev);
	}

	/*
	 * use position update IPC if either it is forced
	 * or we don't have other choice
	 */
#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_FORCE_IPC_POSITION)
	hdev->no_ipc_position = 0;
#else
	hdev->no_ipc_position = sof_ops(sdev)->pcm_pointer ? 1 : 0;
#endif

	/* set up HDA base */
	bus = sof_to_bus(sdev);
	ret = hda_init(sdev);
	if (ret < 0)
		goto hdac_bus_unmap;

	/* DSP base */
	sdev->bar[HDA_DSP_BAR] = pci_ioremap_bar(pci, HDA_DSP_BAR);
	if (!sdev->bar[HDA_DSP_BAR]) {
		dev_err(sdev->dev, "error: ioremap error\n");
		ret = -ENXIO;
		goto hdac_bus_unmap;
	}

	sdev->mmio_bar = HDA_DSP_BAR;
	sdev->mailbox_bar = HDA_DSP_BAR;

	/* allow 64bit DMA address if supported by H/W */
	if (!dma_set_mask(&pci->dev, DMA_BIT_MASK(64))) {
		dev_dbg(sdev->dev, "DMA mask is 64 bit\n");
		dma_set_coherent_mask(&pci->dev, DMA_BIT_MASK(64));
	} else {
		dev_dbg(sdev->dev, "DMA mask is 32 bit\n");
		dma_set_mask(&pci->dev, DMA_BIT_MASK(32));
		dma_set_coherent_mask(&pci->dev, DMA_BIT_MASK(32));
	}

	/* init streams */
	ret = hda_dsp_stream_init(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to init streams\n");
		/*
		 * not all errors are due to memory issues, but trying
		 * to free everything does not harm
		 */
		goto free_streams;
	}

	/*
	 * register our IRQ
	 * let's try to enable msi firstly
	 * if it fails, use legacy interrupt mode
	 * TODO: support interrupt mode selection with kernel parameter
	 *       support msi multiple vectors
	 */
	ret = pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI);
	if (ret < 0) {
		dev_info(sdev->dev, "use legacy interrupt mode\n");
		/*
		 * in IO-APIC mode, hda->irq and ipc_irq are using the same
		 * irq number of pci->irq
		 */
		hdev->irq = pci->irq;
		sdev->ipc_irq = pci->irq;
		sdev->msi_enabled = 0;
	} else {
		dev_info(sdev->dev, "use msi interrupt mode\n");
		hdev->irq = pci_irq_vector(pci, 0);
		/* ipc irq number is the same of hda irq */
		sdev->ipc_irq = hdev->irq;
		sdev->msi_enabled = 1;
	}

	dev_dbg(sdev->dev, "using HDA IRQ %d\n", hdev->irq);
	ret = request_threaded_irq(hdev->irq, hda_dsp_stream_interrupt,
				   hda_dsp_stream_threaded_handler,
				   IRQF_SHARED, "AudioHDA", bus);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register HDA IRQ %d\n",
			hdev->irq);
		goto free_irq_vector;
	}

	dev_dbg(sdev->dev, "using IPC IRQ %d\n", sdev->ipc_irq);
	ret = request_threaded_irq(sdev->ipc_irq, hda_dsp_ipc_irq_handler,
				   sof_ops(sdev)->irq_thread, IRQF_SHARED,
				   "AudioDSP", sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register IPC IRQ %d\n",
			sdev->ipc_irq);
		goto free_hda_irq;
	}

	pci_set_master(pci);
	synchronize_irq(pci->irq);

	/*
	 * clear TCSEL to clear playback on some HD Audio
	 * codecs. PCI TCSEL is defined in the Intel manuals.
	 */
	snd_sof_pci_update_bits(sdev, PCI_TCSEL, 0x07, 0);

	/* init HDA capabilities */
	ret = hda_init_caps(sdev);
	if (ret < 0)
		goto free_ipc_irq;

	/* reset HDA controller */
	ret = hda_dsp_ctrl_link_reset(sdev, true);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to reset HDA controller\n");
		goto free_ipc_irq;
	}

	/* exit HDA controller reset */
	ret = hda_dsp_ctrl_link_reset(sdev, false);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to exit HDA controller reset\n");
		goto free_ipc_irq;
	}

	/* clear stream status */
	list_for_each_entry(stream, &bus->stream_list, list) {
		sd_offset = SOF_STREAM_SD_OFFSET(stream);
		snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
					sd_offset +
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

	/* re-enable CGCTL.MISCBDCGE after reset */
	hda_dsp_ctrl_misc_clock_gating(sdev, true);

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

free_ipc_irq:
	free_irq(sdev->ipc_irq, sdev);
free_hda_irq:
	free_irq(hdev->irq, bus);
free_irq_vector:
	if (sdev->msi_enabled)
		pci_free_irq_vectors(pci);
free_streams:
	hda_dsp_stream_free(sdev);
/* dsp_unmap: not currently used */
	iounmap(sdev->bar[HDA_DSP_BAR]);
hdac_bus_unmap:
	iounmap(bus->remap_addr);
err:
	return ret;
}

int hda_dsp_remove(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hda =
		(struct sof_intel_hda_dev *)sdev->pdata->hw_pdata;
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct pci_dev *pci = to_pci_dev(sdev->dev);
	const struct sof_intel_dsp_desc *chip = hda->desc;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	/* codec removal, invoke bus_device_remove */
	snd_hdac_ext_bus_device_remove(bus);
#endif

	if (!IS_ERR_OR_NULL(hda->dmic_dev))
		platform_device_unregister(hda->dmic_dev);

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
	free_irq(hda->irq, bus);
	if (sdev->msi_enabled)
		pci_free_irq_vectors(pci);

	hda_dsp_stream_free(sdev);
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	snd_hdac_link_free_all(bus);
#endif

	iounmap(sdev->bar[HDA_DSP_BAR]);
	iounmap(bus->remap_addr);

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	snd_hdac_ext_bus_exit(bus);
#endif
	hda_codec_i915_exit(sdev);

	return 0;
}

MODULE_LICENSE("Dual BSD/GPL");
