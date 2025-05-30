/* SPDX-License-Identifier: GPL-2.0-only
 * SPDX-FileCopyrightText: Copyright (c) 2020-2025 NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 *
 * tegra210_i2s.h - Definitions for Tegra210 I2S driver
 *
 */

#ifndef __TEGRA210_I2S_H__
#define __TEGRA210_I2S_H__

/* Register offsets from I2S*_BASE */
#define TEGRA210_I2S_RX_ENABLE			0x0
#define TEGRA210_I2S_RX_SOFT_RESET		0x4
#define TEGRA210_I2S_RX_STATUS			0x0c
#define TEGRA210_I2S_RX_INT_STATUS		0x10
#define TEGRA210_I2S_RX_INT_MASK		0x14
#define TEGRA210_I2S_RX_INT_SET			0x18
#define TEGRA210_I2S_RX_INT_CLEAR		0x1c
#define TEGRA210_I2S_RX_CIF_CTRL		0x20
#define TEGRA210_I2S_RX_CTRL			0x24
#define TEGRA210_I2S_RX_SLOT_CTRL		0x28
#define TEGRA210_I2S_RX_CLK_TRIM		0x2c
#define TEGRA210_I2S_RX_CYA			0x30
#define TEGRA210_I2S_RX_CIF_FIFO_STATUS		0x34
#define TEGRA210_I2S_TX_ENABLE			0x40
#define TEGRA210_I2S_TX_SOFT_RESET		0x44
#define TEGRA210_I2S_TX_STATUS			0x4c
#define TEGRA210_I2S_TX_INT_STATUS		0x50
#define TEGRA210_I2S_TX_INT_MASK		0x54
#define TEGRA210_I2S_TX_INT_SET			0x58
#define TEGRA210_I2S_TX_INT_CLEAR		0x5c
#define TEGRA210_I2S_TX_CIF_CTRL		0x60
#define TEGRA210_I2S_TX_CTRL			0x64
#define TEGRA210_I2S_TX_SLOT_CTRL		0x68
#define TEGRA210_I2S_TX_CLK_TRIM		0x6c
#define TEGRA210_I2S_TX_CYA			0x70
#define TEGRA210_I2S_TX_CIF_FIFO_STATUS		0x74
#define TEGRA210_I2S_ENABLE			0x80
#define TEGRA210_I2S_SOFT_RESET			0x84
#define TEGRA210_I2S_CG				0x88
#define TEGRA210_I2S_STATUS			0x8c
#define TEGRA210_I2S_INT_STATUS			0x90
#define TEGRA210_I2S_CTRL			0xa0
#define TEGRA210_I2S_TIMING			0xa4
#define TEGRA210_I2S_SLOT_CTRL			0xa8
#define TEGRA210_I2S_CLK_TRIM			0xac
#define TEGRA210_I2S_CYA			0xb0

/* T264 specific registers */
#define TEGRA264_I2S_RX_FIFO_WR_ACCESS_MODE	0x30
#define TEGRA264_I2S_RX_CYA			0x3c
#define TEGRA264_I2S_RX_CIF_FIFO_STATUS		0x40
#define TEGRA264_I2S_TX_ENABLE			0x80
#define TEGRA264_I2S_TX_SOFT_RESET		0x84
#define TEGRA264_I2S_TX_STATUS			0x8c
#define TEGRA264_I2S_TX_INT_STATUS		0x90
#define TEGRA264_I2S_TX_INT_MASK		0x94
#define TEGRA264_I2S_TX_CIF_CTRL		0xa0
#define TEGRA264_I2S_TX_FIFO_RD_ACCESS_MODE	0xb0
#define TEGRA264_I2S_TX_FIFO_RD_DATA		0xb4
#define TEGRA264_I2S_TX_FIFO_THRESHOLD		0xb8
#define TEGRA264_I2S_TX_CYA			0xbc
#define TEGRA264_I2S_TX_CIF_FIFO_STATUS		0xc0
#define TEGRA264_I2S_ENABLE			0x100
#define TEGRA264_I2S_CG				0x108
#define TEGRA264_I2S_STATUS			0x10c
#define TEGRA264_I2S_INT_STATUS			0x110
#define TEGRA264_I2S_INT_SET			0x114
#define TEGRA264_I2S_INT_MASK			0x11c
#define TEGRA264_I2S_CTRL			0x12c
#define TEGRA264_I2S_TIMING			0x130
#define TEGRA264_I2S_CYA			0x13c
#define TEGRA264_I2S_PIO_MODE_ENABLE		0x140
#define TEGRA264_I2S_PAD_MACRO_STATUS		0x144

/* Bit fields, shifts and masks */
#define I2S_DATA_SHIFT				8
#define I2S_CTRL_DATA_OFFSET_MASK		(0x7ff << I2S_DATA_SHIFT)
#define TEGRA264_I2S_FSYNC_WIDTH_SHIFT		23
#define TEGRA264_I2S_CTRL_FSYNC_WIDTH_MASK	(0x1ff << TEGRA264_I2S_FSYNC_WIDTH_SHIFT)

#define I2S_EN_SHIFT				0
#define I2S_EN_MASK				BIT(I2S_EN_SHIFT)
#define I2S_EN					BIT(I2S_EN_SHIFT)

#define I2S_FSYNC_WIDTH_SHIFT			24
#define I2S_CTRL_FSYNC_WIDTH_MASK		(0xff << I2S_FSYNC_WIDTH_SHIFT)

#define I2S_POS_EDGE				0
#define I2S_NEG_EDGE				1
#define I2S_EDGE_SHIFT				20
#define I2S_CTRL_EDGE_CTRL_MASK			BIT(I2S_EDGE_SHIFT)
#define I2S_CTRL_EDGE_CTRL_POS_EDGE		(I2S_POS_EDGE << I2S_EDGE_SHIFT)
#define I2S_CTRL_EDGE_CTRL_NEG_EDGE		(I2S_NEG_EDGE << I2S_EDGE_SHIFT)

#define I2S_FMT_LRCK				0
#define I2S_FMT_FSYNC				1
#define I2S_FMT_SHIFT				12
#define I2S_CTRL_FRAME_FMT_MASK			(7 << I2S_FMT_SHIFT)
#define I2S_CTRL_FRAME_FMT_LRCK_MODE		(I2S_FMT_LRCK << I2S_FMT_SHIFT)
#define I2S_CTRL_FRAME_FMT_FSYNC_MODE		(I2S_FMT_FSYNC << I2S_FMT_SHIFT)

#define I2S_CTRL_MASTER_EN_SHIFT		10
#define I2S_CTRL_MASTER_EN_MASK			BIT(I2S_CTRL_MASTER_EN_SHIFT)
#define I2S_CTRL_MASTER_EN			BIT(I2S_CTRL_MASTER_EN_SHIFT)

#define I2S_CTRL_LRCK_POL_SHIFT			9
#define I2S_CTRL_LRCK_POL_MASK			BIT(I2S_CTRL_LRCK_POL_SHIFT)
#define I2S_CTRL_LRCK_POL_LOW			(0 << I2S_CTRL_LRCK_POL_SHIFT)
#define I2S_CTRL_LRCK_POL_HIGH			BIT(I2S_CTRL_LRCK_POL_SHIFT)

#define I2S_CTRL_LPBK_SHIFT			8
#define I2S_CTRL_LPBK_MASK			BIT(I2S_CTRL_LPBK_SHIFT)
#define I2S_CTRL_LPBK_EN			BIT(I2S_CTRL_LPBK_SHIFT)

#define I2S_BITS_8				1
#define I2S_BITS_16				3
#define I2S_BITS_24				5
#define I2S_BITS_32				7
#define I2S_CTRL_BIT_SIZE_MASK			0x7

#define I2S_TIMING_CH_BIT_CNT_MASK		0x7ff
#define I2S_TIMING_CH_BIT_CNT_SHIFT		0

#define I2S_SOFT_RESET_SHIFT			0
#define I2S_SOFT_RESET_MASK			BIT(I2S_SOFT_RESET_SHIFT)
#define I2S_SOFT_RESET_EN			BIT(I2S_SOFT_RESET_SHIFT)

#define I2S_RX_FIFO_DEPTH			64
#define DEFAULT_I2S_RX_FIFO_THRESHOLD		3

#define DEFAULT_I2S_SLOT_MASK			0xffff
#define TEGRA210_I2S_TX_OFFSET			0
#define TEGRA210_I2S_CTRL_OFFSET		0
#define TEGRA210_I2S_MAX_CHANNEL		16

#define TEGRA264_DEFAULT_I2S_SLOT_MASK		0xffffffff
#define TEGRA264_I2S_TX_OFFSET			0x40
#define TEGRA264_I2S_CTRL_OFFSET		0x8c
#define TEGRA264_I2S_MAX_CHANNEL		32

enum tegra210_i2s_path {
	I2S_RX_PATH,
	I2S_TX_PATH,
	I2S_PATHS,
};

struct tegra_i2s_soc_data {
	const struct regmap_config *regmap_conf;
	const struct snd_soc_component_driver *i2s_cmpnt;
	unsigned int max_ch;
	unsigned int tx_offset;
	unsigned int i2s_ctrl_offset;
	unsigned int fsync_width_mask;
	unsigned int fsync_width_shift;
	unsigned int slot_mask;
};

struct tegra210_i2s {
	const struct tegra_i2s_soc_data *soc_data;
	struct clk *clk_i2s;
	struct clk *clk_sync_input;
	struct regmap *regmap;
	int client_sample_format;
	unsigned int client_channels;
	unsigned int stereo_to_mono[I2S_PATHS];
	unsigned int mono_to_stereo[I2S_PATHS];
	unsigned int dai_fmt;
	unsigned int fsync_width;
	unsigned int bclk_ratio;
	unsigned int tx_mask;
	unsigned int rx_mask;
	unsigned int rx_fifo_th;
	bool loopback;
};

#endif
