// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <dt-bindings/clock/qcom,sm8650-videocc.h>

#include "clk-alpha-pll.h"
#include "clk-branch.h"
#include "clk-rcg.h"
#include "clk-regmap.h"
#include "clk-regmap-divider.h"
#include "common.h"
#include "gdsc.h"
#include "reset.h"

enum {
	DT_BI_TCXO,
};

enum {
	P_BI_TCXO,
	P_VIDEO_CC_PLL0_OUT_MAIN,
	P_VIDEO_CC_PLL1_OUT_MAIN,
};

static const struct pll_vco lucid_ole_vco[] = {
	{ 249600000, 2300000000, 0 },
};

static struct alpha_pll_config video_cc_pll0_config = {
	.l = 0x25,
	.alpha = 0x8000,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00182261,
	.config_ctl_hi1_val = 0x82aa299c,
	.test_ctl_val = 0x00000000,
	.test_ctl_hi_val = 0x00000003,
	.test_ctl_hi1_val = 0x00009000,
	.test_ctl_hi2_val = 0x00000034,
	.user_ctl_val = 0x00000000,
	.user_ctl_hi_val = 0x00000005,
};

static struct clk_alpha_pll video_cc_pll0 = {
	.offset = 0x0,
	.config = &video_cc_pll0_config,
	.vco_table = lucid_ole_vco,
	.num_vco = ARRAY_SIZE(lucid_ole_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_OLE],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_pll0",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_BI_TCXO,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_evo_ops,
		},
	},
};

static struct alpha_pll_config video_cc_pll1_config = {
	.l = 0x36,
	.alpha = 0xb000,
	.config_ctl_val = 0x20485699,
	.config_ctl_hi_val = 0x00182261,
	.config_ctl_hi1_val = 0x82aa299c,
	.test_ctl_val = 0x00000000,
	.test_ctl_hi_val = 0x00000003,
	.test_ctl_hi1_val = 0x00009000,
	.test_ctl_hi2_val = 0x00000034,
	.user_ctl_val = 0x00000000,
	.user_ctl_hi_val = 0x00000005,
};

static struct clk_alpha_pll video_cc_pll1 = {
	.offset = 0x1000,
	.config = &video_cc_pll1_config,
	.vco_table = lucid_ole_vco,
	.num_vco = ARRAY_SIZE(lucid_ole_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_OLE],
	.clkr = {
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_pll1",
			.parent_data = &(const struct clk_parent_data) {
				.index = DT_BI_TCXO,
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_evo_ops,
		},
	},
};

static const struct parent_map video_cc_parent_map_0[] = {
	{ P_BI_TCXO, 0 },
	{ P_VIDEO_CC_PLL0_OUT_MAIN, 1 },
};

static const struct clk_parent_data video_cc_parent_data_0[] = {
	{ .index = DT_BI_TCXO },
	{ .hw = &video_cc_pll0.clkr.hw },
};

static const struct parent_map video_cc_parent_map_1[] = {
	{ P_BI_TCXO, 0 },
	{ P_VIDEO_CC_PLL1_OUT_MAIN, 1 },
};

static const struct clk_parent_data video_cc_parent_data_1[] = {
	{ .index = DT_BI_TCXO },
	{ .hw = &video_cc_pll1.clkr.hw },
};

static const struct parent_map video_cc_parent_map_2[] = {
	{ P_BI_TCXO, 0 },
};

static const struct clk_parent_data video_cc_parent_data_2[] = {
	{ .index = DT_BI_TCXO },
};

static const struct freq_tbl ftbl_video_cc_mvs0_clk_src[] = {
	F(720000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1014000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1098000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1332000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1600000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	{ }
};

static const struct freq_tbl ftbl_video_cc_mvs0_clk_src_sm8650[] = {
	F(588000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(900000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1140000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1305000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1440000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1600000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	{ }
};

static const struct freq_tbl ftbl_video_cc_mvs0_clk_src_x1e80100[] = {
	F(576000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(720000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1014000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1098000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1332000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	F(1443000000, P_VIDEO_CC_PLL0_OUT_MAIN, 1, 0, 0),
	{ }
};

static struct clk_rcg2 video_cc_mvs0_clk_src = {
	.cmd_rcgr = 0x8000,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = video_cc_parent_map_0,
	.freq_tbl = ftbl_video_cc_mvs0_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "video_cc_mvs0_clk_src",
		.parent_data = video_cc_parent_data_0,
		.num_parents = ARRAY_SIZE(video_cc_parent_data_0),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static const struct freq_tbl ftbl_video_cc_mvs1_clk_src[] = {
	F(1050000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1350000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1500000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1650000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	{ }
};

static const struct freq_tbl ftbl_video_cc_mvs1_clk_src_sm8650[] = {
	F(840000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1110000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1350000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1500000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1650000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	{ }
};

static const struct freq_tbl ftbl_video_cc_mvs1_clk_src_x1e80100[] = {
	F(840000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1050000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1350000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1500000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	F(1650000000, P_VIDEO_CC_PLL1_OUT_MAIN, 1, 0, 0),
	{ }
};

static struct clk_rcg2 video_cc_mvs1_clk_src = {
	.cmd_rcgr = 0x8018,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = video_cc_parent_map_1,
	.freq_tbl = ftbl_video_cc_mvs1_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "video_cc_mvs1_clk_src",
		.parent_data = video_cc_parent_data_1,
		.num_parents = ARRAY_SIZE(video_cc_parent_data_1),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static const struct freq_tbl ftbl_video_cc_xo_clk_src[] = {
	F(19200000, P_BI_TCXO, 1, 0, 0),
	{ }
};

static struct clk_rcg2 video_cc_xo_clk_src = {
	.cmd_rcgr = 0x810c,
	.mnd_width = 0,
	.hid_width = 5,
	.parent_map = video_cc_parent_map_2,
	.freq_tbl = ftbl_video_cc_xo_clk_src,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "video_cc_xo_clk_src",
		.parent_data = video_cc_parent_data_2,
		.num_parents = ARRAY_SIZE(video_cc_parent_data_2),
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_rcg2_shared_ops,
	},
};

static struct clk_regmap_div video_cc_mvs0_div_clk_src = {
	.reg = 0x80c4,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "video_cc_mvs0_div_clk_src",
		.parent_hws = (const struct clk_hw*[]) {
			&video_cc_mvs0_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ro_ops,
	},
};

static struct clk_regmap_div video_cc_mvs0c_div2_div_clk_src = {
	.reg = 0x8070,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "video_cc_mvs0c_div2_div_clk_src",
		.parent_hws = (const struct clk_hw*[]) {
			&video_cc_mvs0_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ro_ops,
	},
};

static struct clk_regmap_div video_cc_mvs1_div_clk_src = {
	.reg = 0x80ec,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "video_cc_mvs1_div_clk_src",
		.parent_hws = (const struct clk_hw*[]) {
			&video_cc_mvs1_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ro_ops,
	},
};

static struct clk_regmap_div video_cc_mvs1c_div2_div_clk_src = {
	.reg = 0x809c,
	.shift = 0,
	.width = 4,
	.clkr.hw.init = &(const struct clk_init_data) {
		.name = "video_cc_mvs1c_div2_div_clk_src",
		.parent_hws = (const struct clk_hw*[]) {
			&video_cc_mvs1_clk_src.clkr.hw,
		},
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &clk_regmap_div_ro_ops,
	},
};

static struct clk_branch video_cc_mvs0_clk = {
	.halt_reg = 0x80b8,
	.halt_check = BRANCH_HALT_SKIP,
	.hwcg_reg = 0x80b8,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x80b8,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs0_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_mvs0_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch video_cc_mvs0_shift_clk = {
	.halt_reg = 0x8128,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x8128,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x8128,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs0_shift_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_xo_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch video_cc_mvs0c_clk = {
	.halt_reg = 0x8064,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8064,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs0c_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_mvs0c_div2_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch video_cc_mvs0c_shift_clk = {
	.halt_reg = 0x812c,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x812c,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x812c,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs0c_shift_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_xo_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch video_cc_mvs1_clk = {
	.halt_reg = 0x80e0,
	.halt_check = BRANCH_HALT_SKIP,
	.hwcg_reg = 0x80e0,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x80e0,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs1_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_mvs1_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch video_cc_mvs1_shift_clk = {
	.halt_reg = 0x8130,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x8130,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x8130,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs1_shift_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_xo_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch video_cc_mvs1c_clk = {
	.halt_reg = 0x8090,
	.halt_check = BRANCH_HALT,
	.clkr = {
		.enable_reg = 0x8090,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs1c_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_mvs1c_div2_div_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct clk_branch video_cc_mvs1c_shift_clk = {
	.halt_reg = 0x8134,
	.halt_check = BRANCH_HALT_VOTED,
	.hwcg_reg = 0x8134,
	.hwcg_bit = 1,
	.clkr = {
		.enable_reg = 0x8134,
		.enable_mask = BIT(0),
		.hw.init = &(const struct clk_init_data) {
			.name = "video_cc_mvs1c_shift_clk",
			.parent_hws = (const struct clk_hw*[]) {
				&video_cc_xo_clk_src.clkr.hw,
			},
			.num_parents = 1,
			.flags = CLK_SET_RATE_PARENT,
			.ops = &clk_branch2_ops,
		},
	},
};

static struct gdsc video_cc_mvs0c_gdsc = {
	.gdscr = 0x804c,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0x6,
	.pd = {
		.name = "video_cc_mvs0c_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = POLL_CFG_GDSCR | RETAIN_FF_ENABLE,
};

static struct gdsc video_cc_mvs0_gdsc = {
	.gdscr = 0x80a4,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0x6,
	.pd = {
		.name = "video_cc_mvs0_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.parent = &video_cc_mvs0c_gdsc.pd,
	.flags = POLL_CFG_GDSCR | RETAIN_FF_ENABLE | HW_CTRL_TRIGGER,
};

static struct gdsc video_cc_mvs1c_gdsc = {
	.gdscr = 0x8078,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0x6,
	.pd = {
		.name = "video_cc_mvs1c_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.flags = POLL_CFG_GDSCR | RETAIN_FF_ENABLE,
};

static struct gdsc video_cc_mvs1_gdsc = {
	.gdscr = 0x80cc,
	.en_rest_wait_val = 0x2,
	.en_few_wait_val = 0x2,
	.clk_dis_wait_val = 0x6,
	.pd = {
		.name = "video_cc_mvs1_gdsc",
	},
	.pwrsts = PWRSTS_OFF_ON,
	.parent = &video_cc_mvs1c_gdsc.pd,
	.flags = POLL_CFG_GDSCR | RETAIN_FF_ENABLE | HW_CTRL_TRIGGER,
};

static struct clk_regmap *video_cc_sm8550_clocks[] = {
	[VIDEO_CC_MVS0_CLK] = &video_cc_mvs0_clk.clkr,
	[VIDEO_CC_MVS0_CLK_SRC] = &video_cc_mvs0_clk_src.clkr,
	[VIDEO_CC_MVS0_DIV_CLK_SRC] = &video_cc_mvs0_div_clk_src.clkr,
	[VIDEO_CC_MVS0C_CLK] = &video_cc_mvs0c_clk.clkr,
	[VIDEO_CC_MVS0C_DIV2_DIV_CLK_SRC] = &video_cc_mvs0c_div2_div_clk_src.clkr,
	[VIDEO_CC_MVS1_CLK] = &video_cc_mvs1_clk.clkr,
	[VIDEO_CC_MVS1_CLK_SRC] = &video_cc_mvs1_clk_src.clkr,
	[VIDEO_CC_MVS1_DIV_CLK_SRC] = &video_cc_mvs1_div_clk_src.clkr,
	[VIDEO_CC_MVS1C_CLK] = &video_cc_mvs1c_clk.clkr,
	[VIDEO_CC_MVS1C_DIV2_DIV_CLK_SRC] = &video_cc_mvs1c_div2_div_clk_src.clkr,
	[VIDEO_CC_PLL0] = &video_cc_pll0.clkr,
	[VIDEO_CC_PLL1] = &video_cc_pll1.clkr,
	[VIDEO_CC_XO_CLK_SRC] = NULL,
};

static struct gdsc *video_cc_sm8550_gdscs[] = {
	[VIDEO_CC_MVS0C_GDSC] = &video_cc_mvs0c_gdsc,
	[VIDEO_CC_MVS0_GDSC] = &video_cc_mvs0_gdsc,
	[VIDEO_CC_MVS1C_GDSC] = &video_cc_mvs1c_gdsc,
	[VIDEO_CC_MVS1_GDSC] = &video_cc_mvs1_gdsc,
};

static const struct qcom_reset_map video_cc_sm8550_resets[] = {
	[CVP_VIDEO_CC_INTERFACE_BCR] = { 0x80f0 },
	[CVP_VIDEO_CC_MVS0_BCR] = { 0x80a0 },
	[CVP_VIDEO_CC_MVS0C_BCR] = { 0x8048 },
	[CVP_VIDEO_CC_MVS1_BCR] = { 0x80c8 },
	[CVP_VIDEO_CC_MVS1C_BCR] = { 0x8074 },
	[VIDEO_CC_MVS0C_CLK_ARES] = { .reg = 0x8064, .bit = 2, .udelay = 1000 },
	[VIDEO_CC_MVS1C_CLK_ARES] = { .reg = 0x8090, .bit = 2, .udelay = 1000 },
	[VIDEO_CC_XO_CLK_ARES] = { .reg = 0x8124, .bit = 2, .udelay = 100 },
};

static struct clk_alpha_pll *video_cc_sm8550_plls[] = {
	&video_cc_pll0,
	&video_cc_pll1,
};

static u32 video_cc_sm8550_critical_cbcrs[] = {
	0x80f4, /* VIDEO_CC_AHB_CLK */
	0x8124, /* VIDEO_CC_XO_CLK */
	0x8140, /* VIDEO_CC_SLEEP_CLK */
};

static u32 video_cc_sm8650_critical_cbcrs[] = {
	0x80f4, /* VIDEO_CC_AHB_CLK */
	0x8124, /* VIDEO_CC_XO_CLK */
	0x8150, /* VIDEO_CC_SLEEP_CLK */
};

static const struct regmap_config video_cc_sm8550_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0x9f4c,
	.fast_io = true,
};

static struct qcom_cc_driver_data video_cc_sm8550_driver_data = {
	.alpha_plls = video_cc_sm8550_plls,
	.num_alpha_plls = ARRAY_SIZE(video_cc_sm8550_plls),
	.clk_cbcrs = video_cc_sm8550_critical_cbcrs,
	.num_clk_cbcrs = ARRAY_SIZE(video_cc_sm8550_critical_cbcrs),
};

static const struct qcom_cc_desc video_cc_sm8550_desc = {
	.config = &video_cc_sm8550_regmap_config,
	.clks = video_cc_sm8550_clocks,
	.num_clks = ARRAY_SIZE(video_cc_sm8550_clocks),
	.resets = video_cc_sm8550_resets,
	.num_resets = ARRAY_SIZE(video_cc_sm8550_resets),
	.gdscs = video_cc_sm8550_gdscs,
	.num_gdscs = ARRAY_SIZE(video_cc_sm8550_gdscs),
	.use_rpm = true,
	.driver_data = &video_cc_sm8550_driver_data,
};

static const struct of_device_id video_cc_sm8550_match_table[] = {
	{ .compatible = "qcom,sm8550-videocc" },
	{ .compatible = "qcom,sm8650-videocc" },
	{ .compatible = "qcom,x1e80100-videocc" },
	{ }
};
MODULE_DEVICE_TABLE(of, video_cc_sm8550_match_table);

static int video_cc_sm8550_probe(struct platform_device *pdev)
{
	if (of_device_is_compatible(pdev->dev.of_node, "qcom,x1e80100-videocc")) {
		video_cc_pll0_config.l = 0x1e;
		video_cc_pll0_config.alpha = 0x0000;
		video_cc_pll1_config.l = 0x2b;
		video_cc_pll1_config.alpha = 0xc000;
		video_cc_mvs0_clk_src.freq_tbl = ftbl_video_cc_mvs0_clk_src_x1e80100;
		video_cc_mvs1_clk_src.freq_tbl = ftbl_video_cc_mvs1_clk_src_x1e80100;
	}

	if (of_device_is_compatible(pdev->dev.of_node, "qcom,sm8650-videocc")) {
		video_cc_pll0_config.l = 0x1e;
		video_cc_pll0_config.alpha = 0xa000;
		video_cc_pll1_config.l = 0x2b;
		video_cc_pll1_config.alpha = 0xc000;
		video_cc_mvs0_clk_src.freq_tbl = ftbl_video_cc_mvs0_clk_src_sm8650;
		video_cc_mvs1_clk_src.freq_tbl = ftbl_video_cc_mvs1_clk_src_sm8650;
		video_cc_sm8550_clocks[VIDEO_CC_MVS0_SHIFT_CLK] = &video_cc_mvs0_shift_clk.clkr;
		video_cc_sm8550_clocks[VIDEO_CC_MVS0C_SHIFT_CLK] = &video_cc_mvs0c_shift_clk.clkr;
		video_cc_sm8550_clocks[VIDEO_CC_MVS1_SHIFT_CLK] = &video_cc_mvs1_shift_clk.clkr;
		video_cc_sm8550_clocks[VIDEO_CC_MVS1C_SHIFT_CLK] = &video_cc_mvs1c_shift_clk.clkr;
		video_cc_sm8550_clocks[VIDEO_CC_XO_CLK_SRC] = &video_cc_xo_clk_src.clkr;

		video_cc_sm8550_driver_data.clk_cbcrs = video_cc_sm8650_critical_cbcrs;
		video_cc_sm8550_driver_data.num_clk_cbcrs =
							ARRAY_SIZE(video_cc_sm8650_critical_cbcrs);
	}

	return qcom_cc_probe(pdev, &video_cc_sm8550_desc);
}

static struct platform_driver video_cc_sm8550_driver = {
	.probe = video_cc_sm8550_probe,
	.driver = {
		.name = "video_cc-sm8550",
		.of_match_table = video_cc_sm8550_match_table,
	},
};

module_platform_driver(video_cc_sm8550_driver);

MODULE_DESCRIPTION("QTI VIDEOCC SM8550 Driver");
MODULE_LICENSE("GPL");
