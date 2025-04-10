// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ALSA Soc PCM3008 codec support
 *
 * Author:	Hugo Villeneuve
 * Copyright (C) 2008 Lyrtech inc
 *
 * Based on AC97 Soc codec, original copyright follow:
 * Copyright 2005 Wolfson Microelectronics PLC.
 *
 * Generic PCM3008 support.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/soc.h>

struct pcm3008 {
	struct gpio_desc *dem0_pin;
	struct gpio_desc *dem1_pin;
	struct gpio_desc *pdad_pin;
	struct gpio_desc *pdda_pin;
};

static int pcm3008_dac_ev(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol,
			  int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct pcm3008 *pcm = component->dev->platform_data;

	gpiod_set_value_cansleep(pcm->pdda_pin,
				 SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static int pcm3008_adc_ev(struct snd_soc_dapm_widget *w,
			  struct snd_kcontrol *kcontrol,
			  int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct pcm3008 *pcm = component->dev->platform_data;

	gpiod_set_value_cansleep(pcm->pdad_pin,
				 SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

static const struct snd_soc_dapm_widget pcm3008_dapm_widgets[] = {
SND_SOC_DAPM_INPUT("VINL"),
SND_SOC_DAPM_INPUT("VINR"),

SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0, pcm3008_dac_ev,
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_ADC_E("ADC", NULL, SND_SOC_NOPM, 0, 0, pcm3008_adc_ev,
		   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

SND_SOC_DAPM_OUTPUT("VOUTL"),
SND_SOC_DAPM_OUTPUT("VOUTR"),
};

static const struct snd_soc_dapm_route pcm3008_dapm_routes[] = {
	{ "PCM3008 Capture", NULL, "ADC" },
	{ "ADC", NULL, "VINL" },
	{ "ADC", NULL, "VINR" },

	{ "DAC", NULL, "PCM3008 Playback" },
	{ "VOUTL", NULL, "DAC" },
	{ "VOUTR", NULL, "DAC" },
};

#define PCM3008_RATES (SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |	\
		       SNDRV_PCM_RATE_48000)

static struct snd_soc_dai_driver pcm3008_dai = {
	.name = "pcm3008-hifi",
	.playback = {
		.stream_name = "PCM3008 Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = PCM3008_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "PCM3008 Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = PCM3008_RATES,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};

static const struct snd_soc_component_driver soc_component_dev_pcm3008 = {
	.dapm_widgets		= pcm3008_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(pcm3008_dapm_widgets),
	.dapm_routes		= pcm3008_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(pcm3008_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
};

static int pcm3008_codec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pcm3008 *pcm;

	pcm = devm_kzalloc(dev, sizeof(*pcm), GFP_KERNEL);
	if (!pcm)
		return -ENOMEM;
	platform_set_drvdata(pdev, pcm);

	/* DEM1  DEM0  DE-EMPHASIS_MODE
	 * Low   Low   De-emphasis 44.1 kHz ON
	 * Low   High  De-emphasis OFF
	 * High  Low   De-emphasis 48 kHz ON
	 * High  High  De-emphasis 32 kHz ON
	 */

	/* Configure DEM0 GPIO (turning OFF DAC De-emphasis). */
	pcm->dem0_pin = devm_gpiod_get(dev, "dem0", GPIOD_OUT_HIGH);
	if (IS_ERR(pcm->dem0_pin))
		return PTR_ERR(pcm->dem0_pin);

	/* Configure DEM1 GPIO (turning OFF DAC De-emphasis). */
	pcm->dem1_pin = devm_gpiod_get(dev, "dem1", GPIOD_OUT_LOW);
	if (IS_ERR(pcm->dem1_pin))
		return PTR_ERR(pcm->dem1_pin);

	/* Configure PDAD GPIO. */
	pcm->pdad_pin = devm_gpiod_get(dev, "pdad", GPIOD_OUT_LOW);
	if (IS_ERR(pcm->pdad_pin))
		return PTR_ERR(pcm->pdad_pin);

	/* Configure PDDA GPIO. */
	pcm->pdda_pin = devm_gpiod_get(dev, "pdda", GPIOD_OUT_LOW);
	if (IS_ERR(pcm->pdda_pin))
		return PTR_ERR(pcm->pdda_pin);

	return devm_snd_soc_register_component(dev,
			&soc_component_dev_pcm3008, &pcm3008_dai, 1);
}

MODULE_ALIAS("platform:pcm3008-codec");

static struct platform_driver pcm3008_codec_driver = {
	.probe		= pcm3008_codec_probe,
	.driver		= {
		.name	= "pcm3008-codec",
	},
};

module_platform_driver(pcm3008_codec_driver);

MODULE_DESCRIPTION("Soc PCM3008 driver");
MODULE_AUTHOR("Hugo Villeneuve");
MODULE_LICENSE("GPL");
