// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Nintendo 3DS Secure Digital Host Controller driver
 *
 *  Copyright (C) 2021 Santiago Herrera
 *
 *  Based on toshsd.c, copyright (C) 2014 Ondrej Zary and 2007 Richard Betts
 */

//#define DRIVER_NAME "3ds-sdhc"
//#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/io.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/interrupt.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>

#include "ctr_sdhc.h"

#define SDHC_ERR_MASK \
	(SDHC_ERR_BAD_CMD | SDHC_ERR_CRC_FAIL | SDHC_ERR_STOP_BIT | \
	 SDHC_ERR_DATATIMEOUT | SDHC_ERR_TX_OVERFLOW | SDHC_ERR_RX_UNDERRUN | \
	 SDHC_ERR_CMD_TIMEOUT | SDHC_ERR_ILLEGAL_ACC)

#define SDHC_IRQMASK \
	(SDHC_STAT_CMDRESPEND | SDHC_STAT_DATA_END | \
	 SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT | \
	 SDHC_ERR_MASK)

#define SDHC_DEFAULT_CARDOPT \
	(SDHC_CARD_OPTION_RETRIES(14) | \
	SDHC_CARD_OPTION_TIMEOUT(14) | \
	SDHC_CARD_OPTION_NOC2)

/* freeze the CLK pin when inactive if running above 5MHz */
#define SDHC_CLKFREEZE_THRESHOLD	(5000000)

static void __ctr_sdhc_set_ios(struct ctr_sdhc *host, struct mmc_ios *ios)
{
	u16 clk_ctl, card_opt;

	if (ios->clock) {
		unsigned int clkdiv = clk_get_rate(host->sdclk) / ios->clock;

		/* get the divider that best achieves the desired clkrate */
		clk_ctl = (clkdiv <= 1) ? 0 : (roundup_pow_of_two(clkdiv) / 4);
		clk_ctl |= SDHC_CARD_CLKCTL_PIN_ENABLE;

		if (ios->clock >= SDHC_CLKFREEZE_THRESHOLD)
			clk_ctl |= SDHC_CARD_CLKCTL_PIN_FREEZE;
	} else {
		clk_ctl = 0;
	}

	switch (ios->bus_width) {
	default:
		dev_err(host->dev, "invalid bus width %d\n", ios->bus_width);
		return;
	case MMC_BUS_WIDTH_1:
		card_opt = SDHC_DEFAULT_CARDOPT | SDHC_CARD_OPTION_1BIT;
		break;
	case MMC_BUS_WIDTH_4:
		card_opt = SDHC_DEFAULT_CARDOPT | SDHC_CARD_OPTION_4BIT;
		break;
	}

	if (ios->power_mode == MMC_POWER_OFF)
		clk_ctl = 0; /* force-disable clock */

	/* set the desired clock divider and card option config */
	ctr_sdhc_set_clk_opt(host, clk_ctl, card_opt);
	mdelay(10);
}

static void ctr_sdhc_finish_request(struct ctr_sdhc *host, int err)
{
	struct mmc_request *mrq = host->mrq;

	if (!mrq)
		return; /* nothing to do if there's no active request */

	if (err < 0 && mrq->cmd)
		mrq->cmd->error = err;

	host->mrq = NULL;
	mmc_request_done(host->mmc, mrq);
}

static void ctr_sdhc_dataend_irq(struct ctr_sdhc *host, u32 irqstat)
{
	struct mmc_data *data = host->mrq->data;

	if (!(irqstat & SDHC_STAT_DATA_END))
		return;

	if (!data) {
		dev_warn(host->dev, "Spurious data end IRQ\n");
		return;
	}

	data->bytes_xfered = data->error ? 0 : (data->blocks * data->blksz);
	dev_dbg(host->dev, "Completed data request xfr=%d\n",
		data->bytes_xfered);

	ctr_sdhc_stop_internal_set(host, 0);
	ctr_sdhc_finish_request(host, data->error);
}

static void ctr_sdhc_data_irq(struct ctr_sdhc *host, u32 irqstat)
{
	u8 *buf;
	int count;
	u32 data32_irq;
	struct mmc_data *data;
	struct sg_mapping_iter *sg_miter;

	/* data available to be sent or received */
	data = host->mrq->data;
	sg_miter = &host->sg_miter;

	if (!data)
		return;

	data32_irq = ctr_sdhc_reg16_get(host, SDHC_DATA32_CTL);
	if (data->flags & MMC_DATA_READ) {
		if (!(data32_irq & SDHC_DATA32_CTL_RXRDY_PENDING))
			return;
	} else {
		if (data32_irq & SDHC_DATA32_CTL_NTXRQ_PENDING)
			return;
	}

	/* no pending blocks, quit */
	if (!sg_miter_next(sg_miter))
		return;

	buf = sg_miter->addr;

	/* always read one block at a time at most */
	count = sg_miter->length;
	if (count > data->blksz)
		count = data->blksz;

	if (data->flags & MMC_DATA_READ) {
		ioread32_rep(host->fifo_port, buf, count >> 2);
	} else {
		iowrite32_rep(host->fifo_port, buf, count >> 2);
	}

	sg_miter->consumed = count;
	sg_miter_stop(sg_miter);
	/* advance through the scattergather list */
}

static void ctr_sdhc_respend_irq(struct ctr_sdhc *host, u32 irqstat)
{
	struct mmc_command *cmd = host->mrq->cmd;

	if (!(irqstat & SDHC_STAT_CMDRESPEND))
		return;

	if (!cmd) {
		dev_err(host->dev, "spurious CMD IRQ: got end of response "
			"but no command is active\n");
		return;
	}

	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136) { /* 136bit response, fill 32 */
			u32 respbuf[4], *resp;
			resp = cmd->resp;

			ctr_sdhc_get_resp(host, respbuf, 4);
			resp[0] = (respbuf[3] << 8) | (respbuf[2] >> 24);
			resp[1] = (respbuf[2] << 8) | (respbuf[1] >> 24);
			resp[2] = (respbuf[1] << 8) | (respbuf[0] >> 24);
			resp[3] = respbuf[0] << 8;
		} else { /* plain 32 bit response */
			ctr_sdhc_get_resp(host, cmd->resp, 1);
		}
	}

	dev_dbg(host->dev, "command IRQ complete %d %d %x\n", cmd->opcode,
		cmd->error, cmd->flags);

	/* finish the request in the data handler if there is any */
	if (host->mrq->data)
		return;
	ctr_sdhc_finish_request(host, 0);
}

static int ctr_sdhc_card_hotplug_irq(struct ctr_sdhc *host, u32 irqstat)
{
	if (!(irqstat & (SDHC_STAT_CARDREMOVE | SDHC_STAT_CARDINSERT)))
		return 0;

	/* finish any pending requests and do a full hw reset */
	ctr_sdhc_reset(host);
	if (!(irqstat & SDHC_STAT_CARDPRESENT))
		ctr_sdhc_finish_request(host, -ENOMEDIUM);
	mmc_detect_change(host->mmc, 1);
	return 1;
}

static irqreturn_t ctr_sdhc_irq_thread(int irq, void *data)
{
	u32 irqstat;
	struct ctr_sdhc *host = data;
	int error = 0, ret = IRQ_HANDLED;

	mutex_lock(&host->lock);

	irqstat = ctr_sdhc_irqstat_get(host);
	dev_dbg(host->dev, "IRQ status: %x\n", irqstat);

	/* immediately acknowledge all pending IRQs */
	ctr_sdhc_irqstat_ack(host, irqstat & SDHC_IRQMASK);

	/* handle any pending hotplug events */
	if (ctr_sdhc_card_hotplug_irq(host, irqstat))
		goto irq_end;

	/* skip the command/data events when there's no active request */
	if (unlikely(host->mrq == NULL))
		goto irq_end;

	if (irqstat & SDHC_ERR_CMD_TIMEOUT) {
		error = -ETIMEDOUT;
	} else if (irqstat & SDHC_ERR_CRC_FAIL) {
		error = -EILSEQ;
	} else if (irqstat & SDHC_ERR_MASK) {
		dev_err(host->dev, "buffer error: %08X\n",
			irqstat & SDHC_ERR_MASK);
		/*dev_err(host->dev, "detail error status %08X\n",
			ioread32(host->regs + SDHC_ERROR_STATUS));*/
		error = -EIO;
	}

	if (error) {
		/* error during transfer */
		struct mmc_command *cmd = host->mrq->cmd;
		if (cmd)
			cmd->error = error;

		if (error != -ETIMEDOUT)
			goto irq_end; /* serious error */
	}

	ctr_sdhc_data_irq(host, irqstat);
	ctr_sdhc_respend_irq(host, irqstat);
	ctr_sdhc_dataend_irq(host, irqstat);

irq_end:
	mutex_unlock(&host->lock);
	return ret;
}


/** Set clock and power state */
static void ctr_sdhc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	mutex_lock(&host->lock);
	__ctr_sdhc_set_ios(host, ios);
	mutex_unlock(&host->lock);
}


/** Write-Protect & Card Detect handling */
static int ctr_sdhc_get_ro(struct mmc_host *mmc)
{
	int stat;
	struct ctr_sdhc *host = mmc_priv(mmc);
	mutex_lock(&host->lock);
	stat = !(ctr_sdhc_irqstat_get(host) & SDHC_STAT_WRITEPROT);
	mutex_unlock(&host->lock);
	return stat;
}

static int __ctr_sdhc_get_cd(struct ctr_sdhc *host)
{
	return !!(ctr_sdhc_irqstat_get(host) & SDHC_STAT_CARDPRESENT);
}

static int ctr_sdhc_get_cd(struct mmc_host *mmc)
{
	int stat;
	struct ctr_sdhc *host = mmc_priv(mmc);
	mutex_lock(&host->lock);
	stat = __ctr_sdhc_get_cd(host);
	mutex_unlock(&host->lock);
	return stat;
}


/** Data and command request issuing */
static void ctr_sdhc_start_data(struct ctr_sdhc *host, struct mmc_data *data)
{
	unsigned int flags = 0;

	dev_dbg(host->dev,
		"setup data transfer: blocksize %08x "
		"nr_blocks %d, offset: %08x\n",
		data->blksz, data->blocks, data->sg->offset);

	if (data->flags & MMC_DATA_READ)
		flags |= SG_MITER_TO_SG;
	else
		flags |= SG_MITER_FROM_SG;

	sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);
	ctr_sdhc_set_blk_len_cnt(host, data->blksz, data->blocks);
}

static void ctr_sdhc_start_mrq(struct ctr_sdhc *host, struct mmc_command *cmd,
			       struct mmc_data *data)
{
	int c = cmd->opcode;

	if (c == MMC_STOP_TRANSMISSION) {
		/*
		 * the hardware supports automatically issuing a
		 * STOP_TRANSMISSION command, so do it and
		 * fake the response to make it look fine
		 */
		ctr_sdhc_stop_internal_set(host, SDHC_STOP_INTERNAL_ISSUE);

		cmd->resp[0] = cmd->opcode;
		cmd->resp[1] = 0;
		cmd->resp[2] = 0;
		cmd->resp[3] = 0;

		ctr_sdhc_finish_request(host, 0);
		return;
	}

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		c |= SDHC_CMDRSP_NONE;
		break;
	case MMC_RSP_R1:
		c |= SDHC_CMDRSP_R1;
		break;
	case MMC_RSP_R1B:
		c |= SDHC_CMDRSP_R1B;
		break;
	case MMC_RSP_R2:
		c |= SDHC_CMDRSP_R2;
		break;
	case MMC_RSP_R3:
		c |= SDHC_CMDRSP_R3;
		break;
	default:
		dev_err(host->dev, "Unknown response type %d\n",
			mmc_resp_type(cmd));
		break;
	}

	/* handle SDIO and APP_CMD cmd bits */
	if (cmd->opcode == SD_IO_RW_DIRECT || cmd->opcode == SD_IO_RW_EXTENDED)
		c |= SDHC_CMD_SECURE;

	if (cmd->opcode == MMC_APP_CMD)
		c |= SDHC_CMDTYPE_APP;

	if (data) {
		/* handle data transfers if present */
		c |= SDHC_CMD_DATA_XFER;

		if (data->blocks > 1) {
			ctr_sdhc_stop_internal_set(host,
				SDHC_STOP_INTERNAL_ENABLE);
			c |= SDHC_CMD_DATA_MULTI;
		}

		if (data->flags & MMC_DATA_READ)
			c |= SDHC_CMD_DATA_READ;

		ctr_sdhc_start_data(host, data);
	}

	ctr_sdhc_send_cmdarg(host, c, cmd->arg);
}

static void ctr_sdhc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct ctr_sdhc *host = mmc_priv(mmc);

	mutex_lock(&host->lock);
	if (!__ctr_sdhc_get_cd(host)) {
		/* card not present, immediately return an error */
		mrq->cmd->error = -ENOMEDIUM;
		mmc_request_done(mmc, mrq);
	} else {
		WARN_ON(host->mrq != NULL);
		/* warn if there's another live transfer */

		host->mrq = mrq;
		ctr_sdhc_start_mrq(host, mrq->cmd, mrq->data);
	}
	mutex_unlock(&host->lock);
}


/* SDIO IRQ support */
static irqreturn_t ctr_sdhc_sdio_irq_thread(int irq, void *data)
{
	irqreturn_t ret = IRQ_NONE;
	struct ctr_sdhc *host = data;

	mutex_lock(&host->lock);
	if (ctr_sdhc_sdioirq_test(host)) {
		mmc_signal_sdio_irq(host->mmc);
		ret = IRQ_HANDLED;
	}
	mutex_unlock(&host->lock);
	return ret;
}

static void ctr_sdhc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct ctr_sdhc *host = mmc_priv(mmc);
	mutex_lock(&host->lock);
	ctr_sdhc_sdioirq_set(host, enable);
	mutex_unlock(&host->lock);
}

static const struct mmc_host_ops ctr_sdhc_ops = {
	.request = ctr_sdhc_request,
	.set_ios = ctr_sdhc_set_ios,
	.get_ro = ctr_sdhc_get_ro,
	.get_cd = ctr_sdhc_get_cd,
	.enable_sdio_irq = ctr_sdhc_enable_sdio_irq,
};

static int ctr_sdhc_probe(struct platform_device *pdev)
{
	int ret;
	u32 fifo_addr;
	struct clk *sdclk;
	struct device *dev;
	struct mmc_host *mmc;
	unsigned long clkrate;
	struct ctr_sdhc *host;

	dev = &pdev->dev;

	sdclk = devm_clk_get(dev, NULL);
	if (IS_ERR(sdclk)) {
		pr_err("no clock provided\n");
		return PTR_ERR(sdclk);
	}

	ret = clk_prepare_enable(sdclk);
	if (ret)
		return ret;

	clkrate = clk_get_rate(sdclk);
	if (!clkrate)
		return -EINVAL;

	if (of_property_read_u32(dev->of_node, "fifo-addr", &fifo_addr))
		return -EINVAL;

	mmc = mmc_alloc_host(sizeof(struct ctr_sdhc), dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	platform_set_drvdata(pdev, host);

	/* set up host data */
	host->dev = dev;
	host->mmc = mmc;
	host->sdclk = sdclk;

	host->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->regs)) {
		ret = -ENOMEM;
		goto free_mmc;
	}

	host->fifo_port = devm_ioremap(dev, fifo_addr, 4);
	if (!host->fifo_port) {
		ret = -ENOMEM;
		goto free_mmc;
	}

	mmc->ops = &ctr_sdhc_ops;
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_MMC_HIGHSPEED |
		    MMC_CAP_SD_HIGHSPEED | MMC_CAP_SDIO_IRQ;
	mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
	mmc->ocr_avail = MMC_VDD_32_33;

	mmc->max_blk_size = 0x200;
	mmc->max_blk_count = 0xFFFF;

	mmc->f_max = clkrate / 2;
	mmc->f_min = clkrate / 512;

	mmc->max_segs = 1;
	mmc->max_seg_size = mmc->max_blk_size * mmc->max_blk_count;
	mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;

	mutex_init(&host->lock);

	ctr_sdhc_reset(host);

	ret = devm_request_threaded_irq(dev, platform_get_irq(pdev, 0),
					NULL, ctr_sdhc_irq_thread,
					IRQF_ONESHOT, dev_name(dev), host);
	if (ret)
		goto free_mmc;

	ret = devm_request_threaded_irq(dev, platform_get_irq(pdev, 1),
					NULL, ctr_sdhc_sdio_irq_thread,
					IRQF_ONESHOT, dev_name(dev), host);
	if (ret)
		goto free_mmc;

	mmc_add_host(mmc);
	pm_suspend_ignore_children(&pdev->dev, 1);
	return 0;

free_mmc:
	mmc_free_host(mmc);
	return ret;
}

static const struct of_device_id ctr_sdhc_of_match[] = {
	{ .compatible = "nintendo," DRIVER_NAME },
	{},
};
MODULE_DEVICE_TABLE(of, ctr_sdhc_of_match);

static struct platform_driver ctr_sdhc_driver = {
	.probe = ctr_sdhc_probe,

	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(ctr_sdhc_of_match),
	},
};

module_platform_driver(ctr_sdhc_driver);

MODULE_DESCRIPTION("Nintendo 3DS SDHC driver");
MODULE_AUTHOR("Santiago Herrera");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
