/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2014 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of Google or the names of contributors or
 * licensors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * GOOGLE INC AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * GOOGLE OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF GOOGLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 *
 * s25f.c - Helper functions for Spansion S25FL and S25FS SPI flash chips.
 * Uses 24 bit addressing for the FS chips and 32 bit addressing for the FL
 * chips (which is required by the overlayed sector size devices).
 * TODO: Implement fancy hybrid sector architecture helpers.
 */

#include <string.h>

#include "chipdrivers.h"
#include "hwaccess.h"
#include "spi.h"

/*
 * RDAR and WRAR are supported on chips which have more than one set of status
 * and control registers and take an address of the register to read/write.
 * WRR, RDSR2, and RDCR are used on chips with a more limited set of control/
 * status registers.
 *
 * WRR is somewhat peculiar. It shares the same opcode as JEDEC_WRSR, and if
 * given one data byte (following the opcode) it acts the same way. If it's
 * given two data bytes, the first data byte overwrites status register 1
 * and the second data byte overwrites config register 1.
 */
#define CMD_RDAR	0x65
#define CMD_WRAR	0x71
#define CMD_WRAR_LEN	5

#define CMD_RSTEN	0x66
#define CMD_RST		0x99

#define CR3NV_ADDR	0x000004
#define CR3NV_20H_NV	(1 << 3)

/* See "Embedded Algorithm Performance Tables for additional timing specs. */
#define T_W		145 * 1000	/* NV register write time (145ms) */
#define T_RPH		35		/* Reset pulse hold time (35us) */
#define S25FS_T_SE	145 * 1000	/* Sector Erase Time (145ms) */

static int s25f_legacy_software_reset(struct flashctx *flash)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= 1,
		.writearr	= (const unsigned char[]){ CMD_RSTEN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 1,
		.writearr	= (const unsigned char[]){ 0xf0 },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution\n", __func__);
		return result;
	}

	/* Allow time for reset command to execute. The datasheet specifies
	 * Trph = 35us, double that to be safe. */
	programmer_delay(T_RPH * 2);

	return 0;
}

/* "Legacy software reset" is disabled by default on S25FS, use this instead. */
int s25fs_software_reset(struct flashctx *flash)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= 1,
		.writearr	= (const unsigned char[]){ CMD_RSTEN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 1,
		.writearr	= (const unsigned char[]){ CMD_RST },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	msg_cdbg("Force resetting SPI chip.\n");
	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution\n", __func__);
		return result;
	}

	/* Allow time for reset command to execute. Double tRPH to be safe. */
	programmer_delay(T_RPH * 2);

	return 0;
}

static int s25f_poll_status(struct flashctx *flash)
{
	uint8_t tmp = spi_read_status_register(flash);

	while (tmp & SPI_SR_WIP) {
		/*
		 * The WIP bit on S25F chips remains set to 1 if erase or
		 * programming errors occur, so we must check for those
		 * errors here. If an error is encountered, do a software
		 * reset to clear WIP and other volatile bits, otherwise
		 * the chip will be unresponsive to further commands.
		 */
		if (tmp & SPI_SR_ERA_ERR) {
			msg_cerr("Erase error occurred\n");
			s25f_legacy_software_reset(flash);
			return -1;
		}

		if (tmp & (1 << 6)) {
			msg_cerr("Programming error occurred\n");
			s25f_legacy_software_reset(flash);
			return -1;
		}

		programmer_delay(1000 * 10);
		tmp = spi_read_status_register(flash);
	}

	return 0;
}

/* "Read Any Register" instruction only supported on S25FS */
static int s25fs_read_cr(struct flashctx *flash, uint32_t addr)
{
	int result;
	uint8_t cfg;
	/* By default, 8 dummy cycles are necessary for variable-latency
	   commands such as RDAR (see CR2NV[3:0]). */
	unsigned char read_cr_cmd[] = {
					CMD_RDAR,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff),
					0x00, 0x00, 0x00, 0x00,
					0x00, 0x00, 0x00, 0x00,
	};

	result = spi_send_command(flash, sizeof(read_cr_cmd), 1, read_cr_cmd, &cfg);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return -1;
	}

	return cfg;
}

/* "Write Any Register" instruction only supported on S25FS */
static int s25fs_write_cr(struct flashctx *flash,
				uint32_t addr, uint8_t data)
{
	int result;
	struct spi_command cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= CMD_WRAR_LEN,
		.writearr	= (const unsigned char[]){
					CMD_WRAR,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff),
					data
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	result = spi_send_multicommand(flash, cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return -1;
	}

	programmer_delay(T_W);
	return s25f_poll_status(flash);
}

static int s25fs_restore_cr3nv(struct flashctx *flash, uint8_t cfg)
{
	int ret = 0;

	msg_cdbg("Restoring CR3NV value to 0x%02x\n", cfg);
	ret |= s25fs_write_cr(flash, CR3NV_ADDR, cfg);
	ret |= s25fs_software_reset(flash);
	return ret;
}

int s25fs_block_erase_d8(struct flashctx *flash,
		unsigned int addr, unsigned int blocklen)
{
	unsigned char cfg;
	int result;
	static int cr3nv_checked = 0;

	struct spi_command erase_cmds[] = {
	{
		.writecnt	= JEDEC_WREN_OUTSIZE,
		.writearr	= (const unsigned char[]){ JEDEC_WREN },
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= JEDEC_BE_D8_OUTSIZE,
		.writearr	= (const unsigned char[]){
					JEDEC_BE_D8,
					(addr >> 16) & 0xff,
					(addr >> 8) & 0xff,
					(addr & 0xff)
				},
		.readcnt	= 0,
		.readarr	= NULL,
	}, {
		.writecnt	= 0,
		.writearr	= NULL,
		.readcnt	= 0,
		.readarr	= NULL,
	}};

	/* Check if hybrid sector architecture is in use and, if so,
	 * switch to uniform sectors. */
	if (!cr3nv_checked) {
		cfg = s25fs_read_cr(flash, CR3NV_ADDR);
		if (!(cfg & CR3NV_20H_NV)) {
			s25fs_write_cr(flash, CR3NV_ADDR, cfg | CR3NV_20H_NV);
			s25fs_software_reset(flash);

			cfg = s25fs_read_cr(flash, CR3NV_ADDR);
			if (!(cfg & CR3NV_20H_NV)) {
				msg_cerr("%s: Unable to enable uniform "
					"block sizes.\n", __func__);
				return 1;
			}

			msg_cdbg("\n%s: CR3NV updated (0x%02x -> 0x%02x)\n",
					__func__, cfg,
					s25fs_read_cr(flash, CR3NV_ADDR));
			/* Restore CR3V when flashrom exits */
			register_chip_restore(s25fs_restore_cr3nv, flash, cfg);
		}

		cr3nv_checked = 1;
	}

	result = spi_send_multicommand(flash, erase_cmds);
	if (result) {
		msg_cerr("%s failed during command execution at address 0x%x\n",
			__func__, addr);
		return result;
	}

	programmer_delay(S25FS_T_SE);
	return s25f_poll_status(flash);
}


int probe_spi_big_spansion(struct flashctx *flash)
{
	static const unsigned char cmd = JEDEC_RDID;
	int ret;
	unsigned char dev_id[6]; /* We care only about 6 first bytes */

	ret = spi_send_command(flash, sizeof(cmd), sizeof(dev_id), &cmd, dev_id);

	if (!ret) {
		unsigned long i;

		for (i = 0; i < sizeof(dev_id); i++)
			msg_gdbg(" 0x%02x", dev_id[i]);
		msg_gdbg(".\n");

		if (dev_id[0] == flash->chip->manufacture_id) {
			union {
				uint8_t array[4];
				uint32_t whole;
			} model_id;

	/*
	 * The structure of the RDID output is as follows:
	 *
	 *     offset   value              meaning
	 *       00h     01h      Manufacturer ID for Spansion
	 *       01h     20h           128 Mb capacity
	 *       01h     02h           256 Mb capacity
	 *       02h     18h           128 Mb capacity
	 *       02h     19h           256 Mb capacity
	 *       03h     4Dh       Full size of the RDID output (ignored)
	 *       04h     00h       FS: 256-kB physical sectors
	 *       04h     01h       FS: 64-kB physical sectors
	 *       04h     00h       FL: 256-kB physical sectors
	 *       04h     01h       FL: Mix of 64-kB and 4KB overlayed sectors
	 *       05h     80h       FL family
	 *       05h     81h       FS family
	 *
	 * Need to use bytes 1, 2, 4, and 5 to properly identify one of eight
	 * possible chips:
	 *
	 * 2 types * 2 possible sizes * 2 possible sector layouts
	 *
	 */
			memcpy(model_id.array, dev_id + 1, 2);
			memcpy(model_id.array + 2, dev_id + 4, 2);
			if (be_to_cpu32(model_id.whole) == flash->chip->model_id)
				return 1;
		}
	}
	return 0;
}
