/*
 * Copyright 2025-2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>

#include <zephyr/mp/core/mp_caps.h>
#include <zephyr/mp/core/mp_pad.h>
#include <zephyr/mp/core/mp_transform.h>
#include <zephyr/mp/core/mp_value.h>

#include <zephyr/mp/zaud/mp_zaud.h>
#include <zephyr/mp/zaud/mp_zaud_gain.h>
#include <zephyr/mp/zaud/mp_zaud_property.h>

LOG_MODULE_REGISTER(mp_zaud_gain, CONFIG_MP_LOG_LEVEL);

#define GAIN_PERCENT_MIN   0
#define GAIN_PERCENT_MAX   1000
#define GAIN_PERCENT_UNITY 100

#define GAIN_UNITY_FIXED            BIT(16) /* 1.0 in Q16.16 */
/* Use a 64-bit intermediate: gain up to 1000% (Q16.16 655360) times a full-scale
 * sample overflows int32.
 */
#define GAIN_MULTIPLY(sample, gain) (((int64_t)(sample) * (gain)) >> 16)

static int32_t percent_to_fixed_gain(int gain_percent)
{
	gain_percent = CLAMP(gain_percent, GAIN_PERCENT_MIN, GAIN_PERCENT_MAX);
	return (gain_percent * GAIN_UNITY_FIXED) / 100;
}

static int mp_zaud_gain_set_property(struct mp_object *obj, uint32_t key, const void *val)
{
	struct mp_zaud_gain *self = MP_ZAUD_GAIN(obj);

	switch (key) {
	case PROP_GAIN:
		self->gain_percent = *(const int *)val;
		self->gain_fixed = percent_to_fixed_gain(self->gain_percent);
		self->mute = (self->gain_percent == 0);
		LOG_DBG("gain set to %d%% (fixed %d)", self->gain_percent, self->gain_fixed);
		return 0;
	default:
		return mp_transform_set_property(obj, key, val);
	}
}

static int mp_zaud_gain_get_property(struct mp_object *obj, uint32_t key, void *val)
{
	struct mp_zaud_gain *self = MP_ZAUD_GAIN(obj);

	switch (key) {
	case PROP_GAIN:
		*(int *)val = self->gain_percent;
		return 0;
	default:
		return mp_transform_get_property(obj, key, val);
	}
}

static void apply_gain_16bit(struct net_buf *buf, int32_t gain_fixed)
{
	int16_t *samples = (int16_t *)buf->data;
	size_t num_samples = buf->len / sizeof(int16_t);

	for (size_t i = 0; i < num_samples; i++) {
		int32_t tmp = GAIN_MULTIPLY(samples[i], gain_fixed);

		samples[i] = (int16_t)CLAMP(tmp, INT16_MIN, INT16_MAX);
	}
}

static bool mp_zaud_gain_chainfn(struct mp_pad *pad, struct net_buf *in_buf,
				 struct net_buf **out_buf)
{
	struct mp_zaud_gain *gain = MP_ZAUD_GAIN(pad->object.container);

	if (in_buf == NULL || in_buf->data == NULL || in_buf->len == 0) {
		LOG_ERR("Invalid buffer received");
		return false;
	}

	if (gain->mute) {
		memset(in_buf->data, 0, in_buf->len);
	} else if (gain->gain_percent != GAIN_PERCENT_UNITY) {
		/* TODO: only 16-bit PCM is supported */
		apply_gain_16bit(in_buf, gain->gain_fixed);
	}

	/* In-place transform: forward the same buffer downstream */
	*out_buf = in_buf;

	return true;
}

static struct mp_caps *mp_zaud_gain_build_caps(void)
{
	struct mp_value *bit_widths = mp_value_new(MP_TYPE_LIST, NULL);

	mp_value_list_append(bit_widths, mp_value_new(MP_TYPE_UINT, MP_ZAUD_BIT_WIDTH_16));

	return mp_caps_new(MP_MEDIA_AUDIO_PCM,
			   MP_CAPS_BITWIDTH, MP_TYPE_LIST, bit_widths,
			   MP_CAPS_INTERLEAVED, MP_TYPE_BOOLEAN, true,
			   MP_CAPS_END);
}

static struct mp_caps *mp_zaud_gain_transform_caps(struct mp_transform *self,
						   enum mp_pad_direction direction,
						   struct mp_caps *incaps)
{
	ARG_UNUSED(self);
	ARG_UNUSED(direction);
	return mp_caps_ref(incaps);
}

static struct mp_caps *mp_zaud_gain_get_caps(struct mp_transform *transform,
					     enum mp_pad_direction direction)
{
	ARG_UNUSED(transform);

	if (direction == MP_PAD_SRC || direction == MP_PAD_SINK) {
		return mp_zaud_gain_build_caps();
	}

	return NULL;
}

static bool mp_zaud_gain_set_caps(struct mp_transform *transform, enum mp_pad_direction direction,
				  struct mp_caps *caps)
{
	return mp_transform_set_caps(transform, direction, caps);
}

void mp_zaud_gain_init(struct mp_element *self)
{
	struct mp_transform *transform = MP_TRANSFORM(self);
	struct mp_zaud_gain *gain = MP_ZAUD_GAIN(self);

	/* Init base class */
	mp_transform_init(self);

	gain->gain_percent = GAIN_PERCENT_UNITY;
	gain->gain_fixed = GAIN_UNITY_FIXED;
	gain->mute = false;

	self->object.set_property = mp_zaud_gain_set_property;
	self->object.get_property = mp_zaud_gain_get_property;

	transform->mode = MP_MODE_INPLACE;
	transform->sinkpad.chainfn = mp_zaud_gain_chainfn;
	transform->transform_caps = mp_zaud_gain_transform_caps;
	transform->get_caps = mp_zaud_gain_get_caps;
	transform->set_caps = mp_zaud_gain_set_caps;

	mp_transform_update_caps(transform, mp_zaud_gain_get_caps(transform, MP_PAD_SINK),
				 mp_zaud_gain_get_caps(transform, MP_PAD_SRC));
}
