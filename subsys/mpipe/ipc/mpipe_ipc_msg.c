/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The plugin protocol's codec.
 *
 * Every layout here is little-endian and fixed-width, and every reserved byte
 * is written as zero and required to be zero on the way in. Two properties
 * follow that a struct sent as itself cannot offer: the peer's compiler cannot
 * change what the bytes mean, and no byte of local memory the sender did not
 * choose to send ends up on the wire.
 *
 * Message layouts, offsets from the start of the frame:
 *
 *   header, all messages   0  u8  version
 *                          1  u8  type
 *                          2  u16 reserved
 *
 *   DATA_BUFFER            4  u32 offset
 *                          8  u32 size
 *                         12  u32 timestamp
 *                         16  u16 buffer_id
 *                         18  u16 generation           = 20 bytes
 *
 *   DATA_RELEASE           4  u16 buffer_id
 *                          6  u16 generation           =  8 bytes
 *
 *   EVENT                  4  u8  event_type
 *                          5  u8  reserved[3]          =  8 bytes
 *
 *   CAPS                   4  u8  media_type_id
 *                          5  u8  flags
 *                          6  u8  num_fields
 *                          7  u8  reserved
 *                          8  field[num_fields]        =  8 + 16n bytes
 *
 *   field, 16 bytes        0  u8  field_id
 *                          1  u8  value_type
 *                          2  u16 reserved
 *                          4  u32 word[3]
 *
 * A field's three words are the value: one scalar in the first, with the other
 * two zero, or min, max and step for a range.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_msg.h>
#include <zephyr/mpipe/mpipe_dispatch.h>
#include <zephyr/mpipe/mpipe_value.h>

#define OFF_VERSION  0U
#define OFF_TYPE     1U
#define OFF_RESERVED 2U

#define DATA_OFF_OFFSET     4U
#define DATA_OFF_SIZE       8U
#define DATA_OFF_TIMESTAMP  12U
#define DATA_OFF_BUFFER_ID  16U
#define DATA_OFF_GENERATION 18U
#define DATA_LEN            20U

#define RELEASE_OFF_BUFFER_ID  4U
#define RELEASE_OFF_GENERATION 6U
#define RELEASE_LEN            8U

#define EVENT_OFF_TYPE     4U
#define EVENT_OFF_RESERVED 5U
#define EVENT_LEN          8U

#define CAPS_OFF_MEDIA_TYPE 4U
#define CAPS_OFF_FLAGS      5U
#define CAPS_OFF_NUM_FIELDS 6U
#define CAPS_OFF_RESERVED   7U
#define CAPS_OFF_FIELDS     8U

#define FIELD_OFF_ID       0U
#define FIELD_OFF_TYPE     1U
#define FIELD_OFF_RESERVED 2U
#define FIELD_OFF_WORDS    4U

/* The three value words, as a field carries them. */
#define FIELD_WORD_SCALAR 0U
#define FIELD_WORD_MIN    0U
#define FIELD_WORD_MAX    1U
#define FIELD_WORD_STEP   2U
#define FIELD_WORDS       3U

BUILD_ASSERT(MPIPE_IPC_WIRE_FIELD_LEN == FIELD_OFF_WORDS + (FIELD_WORDS * sizeof(uint32_t)),
	     "the field record length must match the layout above");

static size_t caps_length(uint8_t num_fields)
{
	return CAPS_OFF_FIELDS + ((size_t)num_fields * MPIPE_IPC_WIRE_FIELD_LEN);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length)
{
	for (size_t i = 0; i < length; i++) {
		if (bytes[i] != 0U) {
			return false;
		}
	}

	return true;
}

/*
 * Which fields belong to which media type. A structure that names a field its
 * own media type has no meaning for is not a format either side can act on.
 */
static bool field_belongs_to(uint8_t media_type, uint8_t field_id)
{
	if (field_id == MPIPE_CAPS_FRAME_INTERVAL) {
		return media_type == MPIPE_MEDIA_AUDIO_PCM || media_type == MPIPE_MEDIA_VIDEO;
	}
	if (media_type == MPIPE_MEDIA_AUDIO_PCM) {
		return IN_RANGE(field_id, MPIPE_CAPS_SAMPLE_RATE, MPIPE_CAPS_INTERLEAVED);
	}
	if (media_type == MPIPE_MEDIA_VIDEO) {
		return IN_RANGE(field_id, MPIPE_CAPS_PIXEL_FORMAT, MPIPE_CAPS_IMAGE_HEIGHT);
	}

	return false;
}

/*
 * What a field is allowed to carry. A capability field means one kind of thing
 * -- a count, a rate, a channel layout -- so a value of another type announces a
 * format the receiving element cannot act on, whatever the wire says.
 */
static bool field_accepts_type(uint8_t field_id, uint8_t value_type)
{
	if (field_id == MPIPE_CAPS_INTERLEAVED) {
		return value_type == MPIPE_TYPE_BOOLEAN;
	}
	if (field_id == MPIPE_CAPS_PIXEL_FORMAT) {
		return value_type == MPIPE_TYPE_UINT;
	}

	return value_type == MPIPE_TYPE_UINT || value_type == MPIPE_TYPE_UINT_RANGE;
}

static int encode_field(uint8_t *dst, uint8_t field_id, const struct mpipe_value *value)
{
	uint32_t words[FIELD_WORDS] = { 0U, 0U, 0U };

	if (!field_accepts_type(field_id, (uint8_t)value->type)) {
		return -EINVAL;
	}

	switch (value->type) {
	case MPIPE_TYPE_BOOLEAN:
		words[FIELD_WORD_SCALAR] = value->v_boolean ? 1U : 0U;
		break;
	case MPIPE_TYPE_INT:
		words[FIELD_WORD_SCALAR] = (uint32_t)value->v_int;
		break;
	case MPIPE_TYPE_UINT:
		words[FIELD_WORD_SCALAR] = value->v_uint;
		break;
	case MPIPE_TYPE_INT_RANGE:
		words[FIELD_WORD_MIN] = (uint32_t)value->range.min.v_int;
		words[FIELD_WORD_MAX] = (uint32_t)value->range.max.v_int;
		words[FIELD_WORD_STEP] = (uint32_t)value->range.step.v_int;
		break;
	case MPIPE_TYPE_UINT_RANGE:
		words[FIELD_WORD_MIN] = value->range.min.v_uint;
		words[FIELD_WORD_MAX] = value->range.max.v_uint;
		words[FIELD_WORD_STEP] = value->range.step.v_uint;
		break;
	default:
		return -EINVAL;
	}

	memset(dst, 0, MPIPE_IPC_WIRE_FIELD_LEN);
	dst[FIELD_OFF_ID] = field_id;
	dst[FIELD_OFF_TYPE] = (uint8_t)value->type;
	for (unsigned int i = 0; i < FIELD_WORDS; i++) {
		sys_put_le32(words[i], &dst[FIELD_OFF_WORDS + (i * sizeof(uint32_t))]);
	}

	return 0;
}

static int decode_field(const uint8_t *src, uint8_t media_type, uint8_t *field_id,
			struct mpipe_value *value)
{
	uint32_t words[FIELD_WORDS];
	uint8_t id = src[FIELD_OFF_ID];
	uint8_t type = src[FIELD_OFF_TYPE];

	if (!bytes_are_zero(&src[FIELD_OFF_RESERVED], FIELD_OFF_WORDS - FIELD_OFF_RESERVED)) {
		return -EPROTO;
	}
	if (id >= MPIPE_CAPS_END || !field_belongs_to(media_type, id) ||
	    !field_accepts_type(id, type)) {
		return -EPROTO;
	}

	for (unsigned int i = 0; i < FIELD_WORDS; i++) {
		words[i] = sys_get_le32(&src[FIELD_OFF_WORDS + (i * sizeof(uint32_t))]);
	}

	/* A scalar leaves two words unused; they carry no meaning but zero. */
	if (type != MPIPE_TYPE_INT_RANGE && type != MPIPE_TYPE_UINT_RANGE &&
	    (words[FIELD_WORD_MAX] != 0U || words[FIELD_WORD_STEP] != 0U)) {
		return -EPROTO;
	}

	memset(value, 0, sizeof(*value));

	switch (type) {
	case MPIPE_TYPE_BOOLEAN:
		if (words[FIELD_WORD_SCALAR] > 1U) {
			return -EPROTO;
		}
		value->type = MPIPE_TYPE_BOOLEAN;
		value->v_boolean = words[FIELD_WORD_SCALAR] == 1U;
		break;
	case MPIPE_TYPE_INT:
		value->type = MPIPE_TYPE_INT;
		value->v_int = (int32_t)words[FIELD_WORD_SCALAR];
		break;
	case MPIPE_TYPE_UINT:
		value->type = MPIPE_TYPE_UINT;
		value->v_uint = words[FIELD_WORD_SCALAR];
		break;
	case MPIPE_TYPE_INT_RANGE:
		if ((int32_t)words[FIELD_WORD_MIN] > (int32_t)words[FIELD_WORD_MAX] ||
		    words[FIELD_WORD_STEP] == 0U) {
			return -EPROTO;
		}
		value->type = MPIPE_TYPE_INT_RANGE;
		value->range.min.v_int = (int32_t)words[FIELD_WORD_MIN];
		value->range.max.v_int = (int32_t)words[FIELD_WORD_MAX];
		value->range.step.v_int = (int32_t)words[FIELD_WORD_STEP];
		break;
	case MPIPE_TYPE_UINT_RANGE:
		if (words[FIELD_WORD_MIN] > words[FIELD_WORD_MAX] ||
		    words[FIELD_WORD_STEP] == 0U) {
			return -EPROTO;
		}
		value->type = MPIPE_TYPE_UINT_RANGE;
		value->range.min.v_uint = words[FIELD_WORD_MIN];
		value->range.max.v_uint = words[FIELD_WORD_MAX];
		value->range.step.v_uint = words[FIELD_WORD_STEP];
		break;
	default:
		return -EPROTO;
	}

	*field_id = id;

	return 0;
}

static bool caps_encodable(const struct mpipe_structure *caps)
{
	return caps->media_type_id < MPIPE_MEDIA_END &&
	       (caps->flags & ~MPIPE_STRUCTURE_FLAG_ANY) == 0U &&
	       caps->num_fields <= MPIPE_IPC_WIRE_MAX_CAPS_FIELDS;
}

static int encode_caps(uint8_t *buf, const struct mpipe_structure *caps)
{
	buf[CAPS_OFF_MEDIA_TYPE] = caps->media_type_id;
	buf[CAPS_OFF_FLAGS] = caps->flags;
	buf[CAPS_OFF_NUM_FIELDS] = caps->num_fields;

	for (uint8_t i = 0; i < caps->num_fields; i++) {
		uint8_t *field = &buf[CAPS_OFF_FIELDS + ((size_t)i * MPIPE_IPC_WIRE_FIELD_LEN)];
		int ret = encode_field(field, caps->ids[i], &caps->values[i]);

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static int decode_caps(struct mpipe_structure *caps, const uint8_t *buf, size_t length)
{
	uint32_t fields_seen = 0U;
	uint8_t media_type;
	uint8_t num_fields;
	uint8_t flags;

	/* Before any of these bytes are read, they have to be bytes we were sent. */
	if (length < CAPS_OFF_FIELDS) {
		return -EMSGSIZE;
	}

	media_type = buf[CAPS_OFF_MEDIA_TYPE];
	flags = buf[CAPS_OFF_FLAGS];
	num_fields = buf[CAPS_OFF_NUM_FIELDS];

	if (num_fields > MPIPE_IPC_WIRE_MAX_CAPS_FIELDS ||
	    num_fields > CONFIG_MPIPE_STRUCTURE_MAX_FIELDS) {
		return -EPROTO;
	}
	if (length != caps_length(num_fields)) {
		return -EMSGSIZE;
	}
	if (buf[CAPS_OFF_RESERVED] != 0U || media_type >= MPIPE_MEDIA_END ||
	    (flags & ~MPIPE_STRUCTURE_FLAG_ANY) != 0U) {
		return -EPROTO;
	}
	/* "Any format" and "no media type" are both statements about nothing. */
	if (((flags & MPIPE_STRUCTURE_FLAG_ANY) != 0U && media_type != MPIPE_MEDIA_UNKNOWN) ||
	    (media_type == MPIPE_MEDIA_UNKNOWN && num_fields != 0U)) {
		return -EPROTO;
	}

	memset(caps, 0, sizeof(*caps));
	caps->media_type_id = media_type;
	caps->flags = flags;
	caps->num_fields = num_fields;

	for (uint8_t i = 0; i < num_fields; i++) {
		const uint8_t *field =
			&buf[CAPS_OFF_FIELDS + ((size_t)i * MPIPE_IPC_WIRE_FIELD_LEN)];
		int ret = decode_field(field, media_type, &caps->ids[i], &caps->values[i]);

		if (ret != 0) {
			return ret;
		}
		/* One value per field, or the structure does not name a format. */
		if ((fields_seen & BIT(caps->ids[i])) != 0U) {
			return -EPROTO;
		}
		fields_seen |= BIT(caps->ids[i]);
	}

	return 0;
}

int mpipe_ipc_msg_encode(void *dst, size_t capacity, const struct mpipe_ipc_msg *msg,
			 size_t *written)
{
	uint8_t *buf = dst;
	size_t total;

	if (dst == NULL || msg == NULL || written == NULL) {
		return -EINVAL;
	}

	switch (msg->type) {
	case MPIPE_IPC_MSG_DATA_BUFFER:
		total = DATA_LEN;
		break;
	case MPIPE_IPC_MSG_DATA_RELEASE:
		total = RELEASE_LEN;
		break;
	case MPIPE_IPC_MSG_EVENT:
		/*
		 * End of stream is the only event defined to cross the link.
		 * Carrying anything else would mean announcing on the far side
		 * an event the near side never resolved -- caps and pool
		 * negotiation are answered by the element that receives them,
		 * and there is no element on this end of the wire to answer.
		 */
		if (msg->event.event_type != MPIPE_DISPATCH_EOS) {
			return -EINVAL;
		}
		total = EVENT_LEN;
		break;
	case MPIPE_IPC_MSG_CAPS:
		if (!caps_encodable(&msg->caps)) {
			return -EINVAL;
		}
		total = caps_length(msg->caps.num_fields);
		break;
	default:
		return -EINVAL;
	}

	if (capacity < total) {
		return -ENOSPC;
	}

	/* Zero first, so no reserved byte can carry anything from the caller. */
	memset(buf, 0, total);
	buf[OFF_VERSION] = MPIPE_IPC_PLUGIN_VERSION;
	buf[OFF_TYPE] = msg->type;

	switch (msg->type) {
	case MPIPE_IPC_MSG_DATA_BUFFER:
		sys_put_le32(msg->data.offset, &buf[DATA_OFF_OFFSET]);
		sys_put_le32(msg->data.size, &buf[DATA_OFF_SIZE]);
		sys_put_le32(msg->data.timestamp, &buf[DATA_OFF_TIMESTAMP]);
		sys_put_le16(msg->data.buffer_id, &buf[DATA_OFF_BUFFER_ID]);
		sys_put_le16(msg->data.generation, &buf[DATA_OFF_GENERATION]);
		break;
	case MPIPE_IPC_MSG_DATA_RELEASE:
		sys_put_le16(msg->release.buffer_id, &buf[RELEASE_OFF_BUFFER_ID]);
		sys_put_le16(msg->release.generation, &buf[RELEASE_OFF_GENERATION]);
		break;
	case MPIPE_IPC_MSG_EVENT:
		buf[EVENT_OFF_TYPE] = msg->event.event_type;
		break;
	case MPIPE_IPC_MSG_CAPS:
	default: {
		/* The switch above rejected every other type, so this is CAPS. */
		int ret = encode_caps(buf, &msg->caps);

		if (ret != 0) {
			return ret;
		}
		break;
	}
	}

	*written = total;

	return 0;
}

int mpipe_ipc_msg_decode(struct mpipe_ipc_msg *msg, const void *src, size_t length)
{
	const uint8_t *buf = src;
	uint8_t type;

	if (msg == NULL || src == NULL) {
		return -EINVAL;
	}
	if (length < MPIPE_IPC_WIRE_HEADER_LEN) {
		return -EMSGSIZE;
	}
	if (buf[OFF_VERSION] != MPIPE_IPC_PLUGIN_VERSION) {
		return -ENOTSUP;
	}
	if (!bytes_are_zero(&buf[OFF_RESERVED], MPIPE_IPC_WIRE_HEADER_LEN - OFF_RESERVED)) {
		return -EPROTO;
	}

	type = buf[OFF_TYPE];

	switch (type) {
	case MPIPE_IPC_MSG_DATA_BUFFER:
		if (length != DATA_LEN) {
			return -EMSGSIZE;
		}
		msg->data.offset = sys_get_le32(&buf[DATA_OFF_OFFSET]);
		msg->data.size = sys_get_le32(&buf[DATA_OFF_SIZE]);
		msg->data.timestamp = sys_get_le32(&buf[DATA_OFF_TIMESTAMP]);
		msg->data.buffer_id = sys_get_le16(&buf[DATA_OFF_BUFFER_ID]);
		msg->data.generation = sys_get_le16(&buf[DATA_OFF_GENERATION]);
		break;
	case MPIPE_IPC_MSG_DATA_RELEASE:
		if (length != RELEASE_LEN) {
			return -EMSGSIZE;
		}
		msg->release.buffer_id = sys_get_le16(&buf[RELEASE_OFF_BUFFER_ID]);
		msg->release.generation = sys_get_le16(&buf[RELEASE_OFF_GENERATION]);
		break;
	case MPIPE_IPC_MSG_EVENT:
		if (length != EVENT_LEN) {
			return -EMSGSIZE;
		}
		if (!bytes_are_zero(&buf[EVENT_OFF_RESERVED], EVENT_LEN - EVENT_OFF_RESERVED)) {
			return -EPROTO;
		}
		if (buf[EVENT_OFF_TYPE] != MPIPE_DISPATCH_EOS) {
			return -EPROTO;
		}
		msg->event.event_type = buf[EVENT_OFF_TYPE];
		break;
	case MPIPE_IPC_MSG_CAPS: {
		int ret = decode_caps(&msg->caps, buf, length);

		if (ret != 0) {
			return ret;
		}
		break;
	}
	default:
		return -ENOTSUP;
	}

	msg->type = type;

	return 0;
}
