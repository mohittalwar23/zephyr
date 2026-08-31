/*
 * Copyright 2026 NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

SECTION_DATA_PROLOGUE(NonCacheable,,SUBALIGN(4))
{
	. = ALIGN(64);
	*(NonCacheable)
} GROUP_DATA_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)
