/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The plugin protocol's codec. These tests are what stands behind the claim
 * that a decoded message needs no further checking: everything the format can
 * state wrongly is rejected here, so the plugin only validates what depends on
 * its own state.
 */

#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zephyr/mpipe/ipc/mpipe_ipc_msg.h>
#include <zephyr/mpipe/mpipe_dispatch.h>

ZTEST_SUITE(mpipe_ipc_msg, NULL, NULL, NULL, NULL, NULL);

#define DATA_LEN    20U
#define RELEASE_LEN 8U
#define EVENT_LEN   8U

/* Poison, so a byte the encoder forgets to write is visible rather than zero. */
static uint8_t frame[MPIPE_IPC_WIRE_MAX_LEN];

static size_t encode(const struct mpipe_ipc_msg *msg)
{
	size_t written;

	memset(frame, 0xaa, sizeof(frame));
	zassert_ok(mpipe_ipc_msg_encode(frame, sizeof(frame), msg, &written));

	return written;
}

/* Build the frame for a one-field audio CAPS, then let the caller corrupt it. */
static size_t encode_one_field_caps(uint8_t field_id, struct mpipe_value value)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_CAPS,
		.caps = {
			.media_type_id = MPIPE_MEDIA_AUDIO_PCM,
			.num_fields = 1U,
		},
	};

	msg.caps.ids[0] = field_id;
	msg.caps.values[0] = value;

	return encode(&msg);
}

ZTEST(mpipe_ipc_msg, test_data_is_twenty_little_endian_bytes)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = {
			.offset = 0x11223344U,
			.size = 0x55667788U,
			.timestamp = 0x99aabbccU,
			.buffer_id = 0xd00dU,
			.generation = 0xbeefU,
		},
	};
	struct mpipe_ipc_msg decoded;

	zassert_equal(encode(&msg), DATA_LEN);
	zassert_equal(frame[0], MPIPE_IPC_PLUGIN_VERSION);
	zassert_equal(frame[1], MPIPE_IPC_MSG_DATA_BUFFER);
	zassert_equal(sys_get_le16(&frame[2]), 0U, "the header's padding must be zero");
	zassert_equal(sys_get_le32(&frame[4]), 0x11223344U);
	zassert_equal(sys_get_le32(&frame[8]), 0x55667788U);
	zassert_equal(sys_get_le32(&frame[12]), 0x99aabbccU);
	zassert_equal(sys_get_le16(&frame[16]), 0xd00dU);
	zassert_equal(sys_get_le16(&frame[18]), 0xbeefU);
	/* Byte order, not host order: the low byte of each field comes first. */
	zassert_equal(frame[4], 0x44U);

	zassert_ok(mpipe_ipc_msg_decode(&decoded, frame, DATA_LEN));
	zassert_equal(decoded.type, MPIPE_IPC_MSG_DATA_BUFFER);
	zassert_equal(decoded.data.offset, msg.data.offset);
	zassert_equal(decoded.data.size, msg.data.size);
	zassert_equal(decoded.data.timestamp, msg.data.timestamp);
	zassert_equal(decoded.data.buffer_id, msg.data.buffer_id);
	zassert_equal(decoded.data.generation, msg.data.generation);
}

ZTEST(mpipe_ipc_msg, test_release_and_event_round_trip)
{
	struct mpipe_ipc_msg release = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 63U, .generation = 0x0102U },
	};
	struct mpipe_ipc_msg event = {
		.type = MPIPE_IPC_MSG_EVENT,
		.event = { .event_type = MPIPE_DISPATCH_EOS },
	};
	struct mpipe_ipc_msg decoded;

	zassert_equal(encode(&release), RELEASE_LEN);
	zassert_ok(mpipe_ipc_msg_decode(&decoded, frame, RELEASE_LEN));
	zassert_equal(decoded.type, MPIPE_IPC_MSG_DATA_RELEASE);
	zassert_equal(decoded.release.buffer_id, 63U);
	zassert_equal(decoded.release.generation, 0x0102U);

	zassert_equal(encode(&event), EVENT_LEN);
	zassert_equal(frame[4], MPIPE_DISPATCH_EOS);
	zassert_ok(mpipe_ipc_msg_decode(&decoded, frame, EVENT_LEN));
	zassert_equal(decoded.type, MPIPE_IPC_MSG_EVENT);
	zassert_equal(decoded.event.event_type, MPIPE_DISPATCH_EOS);
}

ZTEST(mpipe_ipc_msg, test_caps_round_trip_carries_every_value_shape)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_CAPS,
		.caps = {
			.media_type_id = MPIPE_MEDIA_AUDIO_PCM,
			.num_fields = 3U,
		},
	};
	struct mpipe_ipc_msg decoded;
	size_t length;

	msg.caps.ids[0] = MPIPE_CAPS_SAMPLE_RATE;
	msg.caps.values[0] = (struct mpipe_value)MPIPE_VALUE_UINT_RANGE(16000, 48000, 8000);
	msg.caps.ids[1] = MPIPE_CAPS_INTERLEAVED;
	msg.caps.values[1] = (struct mpipe_value)MPIPE_VALUE_BOOLEAN(true);
	msg.caps.ids[2] = MPIPE_CAPS_BITWIDTH;
	msg.caps.values[2] = (struct mpipe_value)MPIPE_VALUE_UINT(32);

	length = encode(&msg);
	zassert_equal(length, 8U + (3U * MPIPE_IPC_WIRE_FIELD_LEN));

	zassert_ok(mpipe_ipc_msg_decode(&decoded, frame, length));
	zassert_equal(decoded.caps.media_type_id, MPIPE_MEDIA_AUDIO_PCM);
	zassert_equal(decoded.caps.num_fields, 3U);
	zassert_equal(decoded.caps.ids[0], MPIPE_CAPS_SAMPLE_RATE);
	zassert_equal(decoded.caps.values[0].type, MPIPE_TYPE_UINT_RANGE);
	zassert_equal(decoded.caps.values[0].range.min.v_uint, 16000U);
	zassert_equal(decoded.caps.values[0].range.max.v_uint, 48000U);
	zassert_equal(decoded.caps.values[0].range.step.v_uint, 8000U);
	zassert_true(decoded.caps.values[1].v_boolean);
	zassert_equal(decoded.caps.values[2].v_uint, 32U);
}

/*
 * A field means one kind of thing. A sample rate is a count of hertz, so a
 * signed value for it is not a rate with a sign -- it is a peer describing a
 * format in terms this one does not share, and there is no safe way to guess
 * which.
 */
ZTEST(mpipe_ipc_msg, test_a_field_must_carry_the_type_it_is_defined_with)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_CAPS,
		.caps = { .media_type_id = MPIPE_MEDIA_AUDIO_PCM, .num_fields = 1U },
	};
	size_t one_field = 8U + MPIPE_IPC_WIRE_FIELD_LEN;
	struct mpipe_ipc_msg decoded;
	size_t written;

	/* Refused on the way out, so this core never sends one. */
	msg.caps.ids[0] = MPIPE_CAPS_SAMPLE_RATE;
	msg.caps.values[0] = (struct mpipe_value)MPIPE_VALUE_INT(-5);
	zassert_equal(mpipe_ipc_msg_encode(frame, sizeof(frame), &msg, &written), -EINVAL);

	msg.caps.values[0] = (struct mpipe_value)MPIPE_VALUE_BOOLEAN(true);
	zassert_equal(mpipe_ipc_msg_encode(frame, sizeof(frame), &msg, &written), -EINVAL);

	/* And refused on the way in, which is the half that matters. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[9] = MPIPE_TYPE_INT;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* Channel layout is the mirror case: a boolean, and only a boolean. */
	(void)encode_one_field_caps(MPIPE_CAPS_INTERLEAVED,
				    (struct mpipe_value)MPIPE_VALUE_BOOLEAN(true));
	frame[9] = MPIPE_TYPE_UINT;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);
}

/*
 * A structure sent as itself puts its padding on the wire, and padding holds
 * whatever the stack held before. Every byte of a frame here is one the encoder
 * chose to write.
 */
ZTEST(mpipe_ipc_msg, test_no_byte_of_a_frame_is_left_unwritten)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_CAPS,
		.caps = { .media_type_id = MPIPE_MEDIA_AUDIO_PCM, .num_fields = 1U },
	};
	size_t length;

	msg.caps.ids[0] = MPIPE_CAPS_SAMPLE_RATE;
	msg.caps.values[0] = (struct mpipe_value)MPIPE_VALUE_UINT(16000);

	length = encode(&msg);
	for (size_t i = 0; i < length; i++) {
		zassert_not_equal(frame[i], 0xaaU, "byte %zu was never written", i);
	}
	/* And nothing beyond the message was touched. */
	zassert_equal(frame[length], 0xaaU);
}

ZTEST(mpipe_ipc_msg, test_a_peer_speaking_another_version_is_refused)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 1U, .generation = 2U },
	};
	struct mpipe_ipc_msg decoded;

	(void)encode(&msg);
	frame[0] = MPIPE_IPC_PLUGIN_VERSION + 1U;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, RELEASE_LEN), -ENOTSUP);

	frame[0] = MPIPE_IPC_PLUGIN_VERSION - 1U;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, RELEASE_LEN), -ENOTSUP);
}

ZTEST(mpipe_ipc_msg, test_reserved_bytes_must_be_zero)
{
	struct mpipe_ipc_msg event = {
		.type = MPIPE_IPC_MSG_EVENT,
		.event = { .event_type = MPIPE_DISPATCH_EOS },
	};
	struct mpipe_ipc_msg decoded;

	(void)encode(&event);
	frame[2] = 1U;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, EVENT_LEN), -EPROTO,
		      "a nonzero header reserved byte was accepted");

	(void)encode(&event);
	frame[7] = 1U;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, EVENT_LEN), -EPROTO,
		      "a nonzero event reserved byte was accepted");

	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[8U + 2U] = 1U;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame,
					   8U + MPIPE_IPC_WIRE_FIELD_LEN),
		      -EPROTO, "a nonzero field reserved byte was accepted");
}

ZTEST(mpipe_ipc_msg, test_each_type_has_exactly_one_length)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_BUFFER,
		.data = { .offset = 0U, .size = 4U, .buffer_id = 0U, .generation = 1U },
	};
	struct mpipe_ipc_msg decoded;

	(void)encode(&msg);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, DATA_LEN - 1U), -EMSGSIZE);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, DATA_LEN + 1U), -EMSGSIZE);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, 0U), -EMSGSIZE);
	zassert_ok(mpipe_ipc_msg_decode(&decoded, frame, DATA_LEN));
}

ZTEST(mpipe_ipc_msg, test_an_unknown_message_type_is_refused)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 1U, .generation = 2U },
	};
	struct mpipe_ipc_msg decoded;
	size_t written;

	(void)encode(&msg);
	frame[1] = MPIPE_IPC_MSG_END;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, RELEASE_LEN), -ENOTSUP);

	msg.type = MPIPE_IPC_MSG_END;
	zassert_equal(mpipe_ipc_msg_encode(frame, sizeof(frame), &msg, &written), -EINVAL);
}

/* EOS is the only event the link carries; see the encoder. */
ZTEST(mpipe_ipc_msg, test_only_end_of_stream_crosses_as_an_event)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_EVENT,
		.event = { .event_type = MPIPE_DISPATCH_CAPS },
	};
	struct mpipe_ipc_msg decoded;
	size_t written;

	zassert_equal(mpipe_ipc_msg_encode(frame, sizeof(frame), &msg, &written), -EINVAL);

	msg.event.event_type = MPIPE_DISPATCH_EOS;
	(void)encode(&msg);
	frame[4] = MPIPE_DISPATCH_BUFFER_POOL;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, EVENT_LEN), -EPROTO);
}

ZTEST(mpipe_ipc_msg, test_a_caps_that_does_not_name_one_format_is_refused)
{
	struct mpipe_ipc_msg decoded;
	size_t one_field = 8U + MPIPE_IPC_WIRE_FIELD_LEN;
	size_t length;

	/* A field belonging to another medium. */
	length = encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				       (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[8] = MPIPE_CAPS_IMAGE_WIDTH;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, length), -EPROTO);

	/* A field identifier that does not exist. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[8] = MPIPE_CAPS_END;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* A value type that does not exist. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[9] = MPIPE_TYPE_COUNT;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* A media type this build does not know. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[4] = MPIPE_MEDIA_END;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* An undefined flag bit. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[5] = BIT(7);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* More fields than there are identifiers to fill them. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[6] = MPIPE_IPC_WIRE_MAX_CAPS_FIELDS + 1U;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);
}

ZTEST(mpipe_ipc_msg, test_a_value_outside_its_own_rules_is_refused)
{
	size_t one_field = 8U + MPIPE_IPC_WIRE_FIELD_LEN;
	struct mpipe_ipc_msg decoded;

	/* A scalar's unused words carry nothing but zero. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	sys_put_le32(1U, &frame[8U + 8U]);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* A range that runs backwards. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT_RANGE(16000, 48000,
									       8000));
	sys_put_le32(48000U, &frame[8U + 4U]);
	sys_put_le32(16000U, &frame[8U + 8U]);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* A range whose step is zero enumerates nothing. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT_RANGE(16000, 48000,
									       8000));
	sys_put_le32(0U, &frame[8U + 12U]);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

	/* Channel layout is a boolean, and a boolean is zero or one. */
	(void)encode_one_field_caps(MPIPE_CAPS_INTERLEAVED,
				    (struct mpipe_value)MPIPE_VALUE_BOOLEAN(true));
	sys_put_le32(2U, &frame[8U + 4U]);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, one_field), -EPROTO);

}

ZTEST(mpipe_ipc_msg, test_any_and_unknown_are_statements_about_nothing)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_CAPS,
		.caps = {
			.media_type_id = MPIPE_MEDIA_UNKNOWN,
			.flags = MPIPE_STRUCTURE_FLAG_ANY,
		},
	};
	struct mpipe_ipc_msg decoded;
	size_t length = encode(&msg);

	zassert_ok(mpipe_ipc_msg_decode(&decoded, frame, length),
		   "'any format' is a format a peer may announce");
	zassert_equal(decoded.caps.flags, MPIPE_STRUCTURE_FLAG_ANY);

	/* But not "any format, which is PCM". */
	frame[4] = MPIPE_MEDIA_AUDIO_PCM;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, length), -EPROTO);

	/* Nor a medium-less structure that carries fields anyway. */
	(void)encode_one_field_caps(MPIPE_CAPS_SAMPLE_RATE,
				    (struct mpipe_value)MPIPE_VALUE_UINT(16000));
	frame[4] = MPIPE_MEDIA_UNKNOWN;
	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame,
					   8U + MPIPE_IPC_WIRE_FIELD_LEN),
		      -EPROTO);
}

ZTEST(mpipe_ipc_msg, test_argument_rules)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_DATA_RELEASE,
		.release = { .buffer_id = 0U, .generation = 1U },
	};
	struct mpipe_ipc_msg decoded;
	size_t written;

	zassert_equal(mpipe_ipc_msg_encode(NULL, sizeof(frame), &msg, &written), -EINVAL);
	zassert_equal(mpipe_ipc_msg_encode(frame, sizeof(frame), NULL, &written), -EINVAL);
	zassert_equal(mpipe_ipc_msg_encode(frame, sizeof(frame), &msg, NULL), -EINVAL);
	zassert_equal(mpipe_ipc_msg_encode(frame, RELEASE_LEN - 1U, &msg, &written), -ENOSPC);
	zassert_ok(mpipe_ipc_msg_encode(frame, RELEASE_LEN, &msg, &written));

	zassert_equal(mpipe_ipc_msg_decode(NULL, frame, RELEASE_LEN), -EINVAL);
	zassert_equal(mpipe_ipc_msg_decode(&decoded, NULL, RELEASE_LEN), -EINVAL);
}

/*
 * A CAPS frame states its own field count, in a byte four bytes past the
 * header. A frame that stops before that byte has to be refused on its length
 * alone: deciding how long the frame should be by reading the count first
 * reads a byte the peer never sent.
 *
 * The truncated copy is what makes that a real test rather than a restatement.
 * Decoding a short length out of the full frame would still find the encoder's
 * own header bytes sitting there, and every path would agree. Here the bytes
 * past the cut belong to nobody.
 */
ZTEST(mpipe_ipc_msg, test_a_caps_shorter_than_its_own_header_is_refused)
{
	struct mpipe_ipc_msg msg = {
		.type = MPIPE_IPC_MSG_CAPS,
		.caps = { .media_type_id = MPIPE_MEDIA_AUDIO_PCM, .num_fields = 1U },
	};
	struct mpipe_ipc_msg decoded;
	size_t length;

	msg.caps.ids[0] = MPIPE_CAPS_SAMPLE_RATE;
	msg.caps.values[0] = (struct mpipe_value)MPIPE_VALUE_UINT(16000);
	length = encode(&msg);

	for (size_t truncated = MPIPE_IPC_WIRE_HEADER_LEN; truncated < length; truncated++) {
		uint8_t received[MPIPE_IPC_WIRE_MAX_LEN];

		memset(received, 0xbb, sizeof(received));
		memcpy(received, frame, truncated);

		zassert_equal(mpipe_ipc_msg_decode(&decoded, received, truncated), -EMSGSIZE,
			      "a %zu-byte CAPS was accepted", truncated);
	}

	zassert_ok(mpipe_ipc_msg_decode(&decoded, frame, length));
}

/*
 * The compatibility case the format exists for. CONFIG_MPIPE_STRUCTURE_MAX_FIELDS
 * is a local storage choice, and the peer's copy of it is unknowable. A peer
 * built with more room than this one can legally put more fields on the wire
 * than this one can hold -- the format's own ceiling is the number of field
 * identifiers that exist, not anybody's Kconfig -- and that must come back as a
 * refusal, not as a structure filled in past its end.
 */
ZTEST(mpipe_ipc_msg, test_a_peer_with_more_field_slots_is_refused_not_misparsed)
{
	const uint8_t over = CONFIG_MPIPE_STRUCTURE_MAX_FIELDS + 1U;
	struct mpipe_ipc_msg decoded;
	size_t length;

	zassert_true(over <= MPIPE_IPC_WIRE_MAX_CAPS_FIELDS,
		     "this test needs a field count the wire allows but we cannot hold");

	/* Built by hand: a structure this build can hold cannot express it. */
	memset(frame, 0, sizeof(frame));
	frame[0] = MPIPE_IPC_PLUGIN_VERSION;
	frame[1] = MPIPE_IPC_MSG_CAPS;
	frame[4] = MPIPE_MEDIA_AUDIO_PCM;
	frame[6] = over;

	/*
	 * Every record is a well-formed one. The count is refused before any of
	 * them is read, which is the property being tested: a structure that
	 * cannot be held is never partly filled in first.
	 */
	for (uint8_t i = 0; i < over; i++) {
		uint8_t *field = &frame[8U + ((size_t)i * MPIPE_IPC_WIRE_FIELD_LEN)];

		field[0] = MPIPE_CAPS_SAMPLE_RATE;
		field[1] = MPIPE_TYPE_UINT;
		sys_put_le32(16000U, &field[4]);
	}
	length = 8U + ((size_t)over * MPIPE_IPC_WIRE_FIELD_LEN);

	zassert_equal(mpipe_ipc_msg_decode(&decoded, frame, length), -EPROTO,
		      "a peer with more field slots was misparsed");
}
