/*
 * Copyright (C) 2010 - 2022 Novatek, Inc.
 *
 * $Revision: 110851 $
 * $Date: 2022-12-27 17:56:41 +0800 (週二, 27 十二月 2022) $
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/firmware.h>
#include <linux/gpio.h>

#include "nt36xxx.h"
#include "novatek_ts_fw_boe.h"

#if BOOT_UPDATE_FIRMWARE || NVT_TOUCH_MP

#if NVT_CUST_PROC_CMD
extern struct work_struct ts_restore_cmd_work;
extern struct workqueue_struct *nvt_fwu_wq;
#endif

#if NVT_SUPPORT_PEN
extern int32_t nvt_support_pen_set(uint8_t status);
#endif

extern void nvt_force_continuous_scan(void);

static ktime_t start, end;
const struct firmware *fw_entry = NULL;
static size_t fw_need_write_size = 0;
static uint8_t *fwbuf = NULL;

struct nvt_ts_bin_map {
	char name[12];
	uint32_t BIN_addr;
	uint32_t SRAM_addr;
	uint32_t size;
	uint32_t crc;
};

static struct nvt_ts_bin_map *bin_map;

static int32_t nvt_get_fw_need_write_size(const struct firmware *fw_entry)
{
	int32_t i = 0;
	int32_t total_sectors_to_check = 0;

	total_sectors_to_check = fw_entry->size / FLASH_SECTOR_SIZE;
	/* printk("total_sectors_to_check = %d\n", total_sectors_to_check); */

	for (i = total_sectors_to_check; i > 0; i--) {
		/* printk("current end flag address checked = 0x%X\n", i * FLASH_SECTOR_SIZE - NVT_FLASH_END_FLAG_LEN); */
		/* check if there is end flag "NVT" at the end of this sector */
		if (strncmp(&fw_entry->data[i * FLASH_SECTOR_SIZE - NVT_FLASH_END_FLAG_LEN], "NVT", NVT_FLASH_END_FLAG_LEN) == 0) {
			fw_need_write_size = i * FLASH_SECTOR_SIZE;
			NVT_LOG("fw_need_write_size = %zu(0x%zx), NVT end flag\n", fw_need_write_size, fw_need_write_size);
			return 0;
		}

		/* check if there is end flag "MOD" at the end of this sector */
		if (strncmp(&fw_entry->data[i * FLASH_SECTOR_SIZE - NVT_FLASH_END_FLAG_LEN], "MOD", NVT_FLASH_END_FLAG_LEN) == 0) {
			fw_need_write_size = i * FLASH_SECTOR_SIZE;
			NVT_LOG("fw_need_write_size = %zu(0x%zx), MOD end flag\n", fw_need_write_size, fw_need_write_size);
			return 0;
		}
	}

	NVT_ERR("end flag \"NVT\" \"MOD\" not found!\n");
	return -1;
}

/*******************************************************
Description:
	Novatek touchscreen init variable and allocate buffer
for download firmware function.

return:
	n.a.
*******************************************************/
static int32_t nvt_download_init(void)
{
	/* allocate buffer for transfer firmware */
	//NVT_LOG("NVT_TRANSFER_LEN = 0x%06X\n", NVT_TRANSFER_LEN);

	if (fwbuf == NULL) {
		fwbuf = (uint8_t *)kzalloc((NVT_TRANSFER_LEN + 1 + DUMMY_BYTES), GFP_KERNEL);
		if(fwbuf == NULL) {
			NVT_ERR("kzalloc for fwbuf failed!\n");
			return -ENOMEM;
		}
	}

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen checksum function. Calculate bin
file checksum for comparison.

return:
	n.a.
*******************************************************/
static uint32_t CheckSum(const u8 *data, size_t len)
{
	uint32_t i = 0;
	uint32_t checksum = 0;

	for (i = 0 ; i < len+1 ; i++)
		checksum += data[i];

	checksum += len;
	checksum = ~checksum +1;

	return checksum;
}

static uint32_t byte_to_word(const uint8_t *data)
{
	return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}

/*******************************************************
Description:
	Novatek touchscreen parsing bin header function.

return:
	n.a.
*******************************************************/
static uint32_t partition = 0;
static uint8_t ilm_dlm_num = 2;
static uint8_t cascade_2nd_header_info = 0;
static uint8_t spi_dma_div_cnt_val = 0;
static int32_t nvt_bin_header_parser(const u8 *fwdata, size_t fwsize)
{
	uint32_t list = 0;
	uint32_t pos = 0x00;
	uint32_t end = 0x00;
	uint8_t info_sec_num = 0;
	uint8_t ovly_sec_num = 0;
	uint8_t ovly_info = 0;
	uint8_t find_bin_header = 0;

	/* Find the header size */
	end = fwdata[0] + (fwdata[1] << 8) + (fwdata[2] << 16) + (fwdata[3] << 24);

	/* check cascade next header */
	cascade_2nd_header_info = (fwdata[0x20] & 0x02) >> 1;
	NVT_LOG("cascade_2nd_header_info = %d\n", cascade_2nd_header_info);

	if (cascade_2nd_header_info) {
		pos = 0x30;	// info section start at 0x30 offset
		while (pos < (end / 2)) {
			info_sec_num ++;
			pos += 0x10;	/* each header info is 16 bytes */
		}

		info_sec_num = info_sec_num + 1; //next header section

		spi_dma_div_cnt_val = fwdata[0x29] & 0x01;
	} else {
		pos = 0x30;	// info section start at 0x30 offset
		while (pos < end) {
			info_sec_num ++;
			pos += 0x10;	/* each header info is 16 bytes */
		}
	}

	/*
	 * Find the DLM OVLY section
	 * [0:3] Overlay Section Number
	 * [4]   Overlay Info
	 */
	ovly_info = (fwdata[0x28] & 0x10) >> 4;
	ovly_sec_num = (ovly_info) ? (fwdata[0x28] & 0x0F) : 0;

	/*
	 * calculate all partition number
	 * ilm_dlm_num (ILM & DLM) + ovly_sec_num + info_sec_num
	 */
	partition = ilm_dlm_num + ovly_sec_num + info_sec_num;
	NVT_LOG("ovly_info = %d, ilm_dlm_num = %d, ovly_sec_num = %d, info_sec_num = %d, partition = %d\n",
			ovly_info, ilm_dlm_num, ovly_sec_num, info_sec_num, partition);

	/* allocated memory for header info */
	bin_map = (struct nvt_ts_bin_map *)kzalloc((partition+1) * sizeof(struct nvt_ts_bin_map), GFP_KERNEL);
	if(bin_map == NULL) {
		NVT_ERR("kzalloc for bin_map failed!\n");
		return -ENOMEM;
	}

	for (list = 0; list < partition; list++) {
		/*
		 * [1] parsing ILM & DLM header info
		 * BIN_addr : SRAM_addr : size (12-bytes)
		 * crc located at 0x18 & 0x1C
		 */
		if (list < ilm_dlm_num) {
			bin_map[list].BIN_addr = byte_to_word(&fwdata[0 + list*12]);
			bin_map[list].SRAM_addr = byte_to_word(&fwdata[4 + list*12]);
			bin_map[list].size = byte_to_word(&fwdata[8 + list*12]);
			if (ts->hw_crc)
				bin_map[list].crc = byte_to_word(&fwdata[0x18 + list*4]);
			else { //ts->hw_crc
				if ((bin_map[list].BIN_addr + bin_map[list].size) < fwsize)
					bin_map[list].crc = CheckSum(&fwdata[bin_map[list].BIN_addr], bin_map[list].size);
				else {
					NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
							bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
					return -EINVAL;
				}
			} //ts->hw_crc
			if (list == 0)
				sprintf(bin_map[list].name, "ILM");
			else if (list == 1)
				sprintf(bin_map[list].name, "DLM");
		}

		/*
		 * [2] parsing others header info
		 * SRAM_addr : size : BIN_addr : crc (16-bytes)
		 */
		if ((list >= ilm_dlm_num) && (list < (ilm_dlm_num + info_sec_num))) {
			if (find_bin_header == 0) {
				/* others partition located at 0x30 offset */
				pos = 0x30 + (0x10 * (list - ilm_dlm_num));
			} else if (find_bin_header && cascade_2nd_header_info) {
				/* cascade 2nd header info */
				pos = end - 0x10;
			}

			bin_map[list].SRAM_addr = byte_to_word(&fwdata[pos]);
			bin_map[list].size = byte_to_word(&fwdata[pos+4]);
			bin_map[list].BIN_addr = byte_to_word(&fwdata[pos+8]);
			if (ts->hw_crc)
				bin_map[list].crc = byte_to_word(&fwdata[pos+12]);
			else { //ts->hw_crc
				if ((bin_map[list].BIN_addr + bin_map[list].size) < fwsize)
					bin_map[list].crc = CheckSum(&fwdata[bin_map[list].BIN_addr], bin_map[list].size);
				else {
					NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
							bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
					return -EINVAL;
				}
			} //ts->hw_crc
			/* detect header end to protect parser function */
			if ((bin_map[list].BIN_addr < end) && (bin_map[list].size != 0)) {
				sprintf(bin_map[list].name, "Header");
				find_bin_header = 1;
			} else {
				sprintf(bin_map[list].name, "Info-%d", (list - ilm_dlm_num));
			}
		}

		/*
		 * [3] parsing overlay section header info
		 * SRAM_addr : size : BIN_addr : crc (16-bytes)
		 */
		if (list >= (ilm_dlm_num + info_sec_num)) {
			/* overlay info located at DLM (list = 1) start addr */
			pos = bin_map[1].BIN_addr + (0x10 * (list- ilm_dlm_num - info_sec_num));

			bin_map[list].SRAM_addr = byte_to_word(&fwdata[pos]);
			bin_map[list].size = byte_to_word(&fwdata[pos+4]);
			bin_map[list].BIN_addr = byte_to_word(&fwdata[pos+8]);
			if (ts->hw_crc)
				bin_map[list].crc = byte_to_word(&fwdata[pos+12]);
			else { //ts->hw_crc
				if ((bin_map[list].BIN_addr + bin_map[list].size) < fwsize)
					bin_map[list].crc = CheckSum(&fwdata[bin_map[list].BIN_addr], bin_map[list].size);
				else {
					NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
							bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
					return -EINVAL;
				}
			} //ts->hw_crc
			sprintf(bin_map[list].name, "Overlay-%d", (list- ilm_dlm_num - info_sec_num));
		}

		/* BIN size error detect */
		if ((bin_map[list].BIN_addr + bin_map[list].size) > fwsize) {
			NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
					bin_map[list].BIN_addr, bin_map[list].BIN_addr + bin_map[list].size);
			return -EINVAL;
		}

		NVT_LOG("[%d][%s] SRAM (0x%08X), SIZE (0x%08X), BIN (0x%08X), CRC (0x%08X)\n",
				list, bin_map[list].name,
				bin_map[list].SRAM_addr, bin_map[list].size,  bin_map[list].BIN_addr, bin_map[list].crc);
	}

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen release update firmware function.

return:
	n.a.
*******************************************************/
static void update_firmware_release(void)
{
	fw_entry = NULL;
}

/*******************************************************
Description:
	Novatek touchscreen request update firmware function.

return:
	Executive outcomes. 0---succeed. -1,-22---failed.
*******************************************************/
static int32_t update_firmware_request(char *filename)
{
	int32_t ret = 0;
	static struct firmware embedded_fw;

	NVT_LOG("Using embedded BOE firmware (size %zu bytes)\n", sizeof(nvt_embedded_fw_boe));
	embedded_fw.data = nvt_embedded_fw_boe;
	embedded_fw.size = sizeof(nvt_embedded_fw_boe);
	fw_entry = &embedded_fw;

	// check FW need to write size
	if (nvt_get_fw_need_write_size(fw_entry)) {
		NVT_ERR("get fw need to write size fail!\n");
		return -EINVAL;
	}

	// check if FW version add FW version bar equals 0xFF
	if (*(fw_entry->data + FW_BIN_VER_OFFSET) + *(fw_entry->data + FW_BIN_VER_BAR_OFFSET) != 0xFF) {
		NVT_ERR("bin file FW_VER + FW_VER_BAR should be 0xFF!\n");
		NVT_ERR("FW_VER=0x%02X, FW_VER_BAR=0x%02X\n", *(fw_entry->data+FW_BIN_VER_OFFSET), *(fw_entry->data+FW_BIN_VER_BAR_OFFSET));
		return -ENOEXEC;
	}

	/* BIN Header Parser */
	ret = nvt_bin_header_parser(fw_entry->data, fw_entry->size);
	if (ret) {
		NVT_ERR("bin header parser failed\n");
		return ret;
	}

	return 0;
}

/*******************************************************
Description:
	Novatek touchscreen write data to sram function.

- fwdata   : The buffer is written
- SRAM_addr: The sram destination address
- size     : Number of data bytes in @fwdata being written
- BIN_addr : The transferred data offset of @fwdata

return:
	Executive outcomes. 0---succeed. else---fail.
*******************************************************/
static int32_t nvt_write_sram(const u8 *fwdata,
		uint32_t SRAM_addr, uint32_t size, uint32_t BIN_addr)
{
	int32_t ret = 0;
	uint32_t i = 0;
	uint16_t len = 0;
	int32_t count = 0;

	if (size % NVT_TRANSFER_LEN)
		count = (size / NVT_TRANSFER_LEN) + 1;
	else
		count = (size / NVT_TRANSFER_LEN);

	for (i = 0 ; i < count ; i++) {
		len = (size < NVT_TRANSFER_LEN) ? size : NVT_TRANSFER_LEN;

		//---set xdata index to start address of SRAM---
		ret = nvt_set_page(SRAM_addr);
		if (ret) {
			NVT_ERR("set page failed, ret = %d\n", ret);
			return ret;
		}

		//---write data into SRAM---
		fwbuf[0] = SRAM_addr & 0x7F;	//offset
		memcpy(fwbuf+1, &fwdata[BIN_addr], len);	//payload
		ret = CTP_SPI_WRITE(ts->client, fwbuf, len+1);
		if (ret) {
			NVT_ERR("write to sram failed, ret = %d\n", ret);
			return ret;
		}

		SRAM_addr += NVT_TRANSFER_LEN;
		BIN_addr += NVT_TRANSFER_LEN;
		size -= NVT_TRANSFER_LEN;
	}

	return ret;
}

/*******************************************************
Description:
	Read back the SRAM firmware partitions just written and
	compare byte-for-byte against the source bin. Distinguishes
	an SPI write-path corruption from a chip-side boot failure.
	return: 0---verified clean.  <0---mismatch found (log-only).
*******************************************************/
static int32_t __maybe_unused nvt_verify_sram(const u8 *fwdata, size_t fwsize)
{
	uint8_t *rbuf;
	uint32_t list = 0;
	uint32_t off = 0;
	uint32_t chunk = 0;
	uint32_t remain = 0;
	uint32_t SRAM_addr = 0;
	uint32_t BIN_addr = 0;
	uint32_t size = 0;
	uint32_t total_bad = 0;
	int32_t ret = 0;

	rbuf = kzalloc(NVT_READ_LEN + 1, GFP_KERNEL);
	if (!rbuf)
		return -ENOMEM;

	for (list = 0; list < partition; list++) {
		SRAM_addr = bin_map[list].SRAM_addr;
		size = bin_map[list].size;
		BIN_addr = bin_map[list].BIN_addr;

		if (!size)
			continue;
		if ((BIN_addr + size) > fwsize)
			continue;

		nvt_crash_append("FWU verify [%02u][%s] SRAM=0x%06X size=0x%X\n",
			list, bin_map[list].name, SRAM_addr, size);

		off = 0;
		while (off < size) {
			chunk = min_t(uint32_t, NVT_READ_LEN, size - off);
			nvt_set_page(SRAM_addr + off);
			memset(rbuf, 0, NVT_READ_LEN + 1);
			rbuf[0] = (SRAM_addr + off) & 0x7F;
			CTP_SPI_READ(ts->client, rbuf, chunk + 1);
			for (remain = 0; remain < chunk; remain++) {
				if (rbuf[1 + remain] != fwdata[BIN_addr + off + remain]) {
					uint32_t in = off + remain;
					uint32_t h2 = in & 0x7FF;
					uint32_t h4 = in & 0xFFF;
					nvt_crash_append("VBD p%u i0x%X o%u h2=%u h4=%u\n",
						list, in, in, h2, h4);
					total_bad++;
				}
			}
			off += chunk;
		}
	}
	nvt_crash_append("FWU verify total=%u\n", total_bad);

	if (total_bad) {
		NVT_ERR("SRAM verify: %u bad bytes\n", total_bad);
		nvt_crash_append("FWU hw-crc: verify %u bad bytes\n", total_bad);
		ret = -1;
	} else {
		NVT_LOG("SRAM verify: OK\n");
		nvt_crash_append("FWU hw-crc: verify OK\n");
	}

	kfree(rbuf);
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen nvt_write_firmware function to write
firmware into each partition.

return:
	n.a.
*******************************************************/
static int32_t nvt_write_firmware(const u8 *fwdata, size_t fwsize)
{
	uint32_t list = 0;
	char *name;
	uint32_t BIN_addr, SRAM_addr, size;
	int32_t ret = 0;

	memset(fwbuf, 0, (NVT_TRANSFER_LEN+1));

	for (list = 0; list < partition; list++) {
		/* initialize variable */
		SRAM_addr = bin_map[list].SRAM_addr;
		size = bin_map[list].size;
		BIN_addr = bin_map[list].BIN_addr;
		name = bin_map[list].name;

//		NVT_LOG("[%d][%s] SRAM (0x%08X), SIZE (0x%08X), BIN (0x%08X)\n",
//				list, name, SRAM_addr, size, BIN_addr);

		/* Check data size */
		if ((BIN_addr + size) > fwsize) {
			NVT_ERR("access range (0x%08X to 0x%08X) is larger than bin size!\n",
					BIN_addr, BIN_addr + size);
			ret = -EINVAL;
			goto out;
		}

		/* ignore reserved partition (Reserved Partition size is zero) */
		if (!size)
			continue;
		else
			size = size +1;

		/* write data to SRAM */
		ret = nvt_write_sram(fwdata, SRAM_addr, size, BIN_addr);
		if (ret) {
			NVT_ERR("sram program failed, ret = %d\n", ret);
			goto out;
		}
	}

out:
	return ret;
}

/*
 * Direct-SRAM firmware writer.  Same chunked 31KB burst as the proven
 * transport probe, but pushes each bin partition straight into its real
 * SRAM address.  Order matters: the bootloader shim runs out of ILM, so
 * scribble DLM first (free scratch), ILM last.
 */
static int32_t __maybe_unused nvt_write_firmware_direct(const u8 *fwdata, size_t fwsize)
{
	uint32_t order[8];
	uint32_t o = 0;
	uint32_t list = 0;
	uint32_t BIN_addr, SRAM_addr, size;
	int32_t ret = 0;

	memset(order, 0, sizeof(order));

	for (list = 0; list < partition; list++) {
		if (!bin_map[list].size)
			continue;
		if (strcmp(bin_map[list].name, "DLM") == 0 && o < 8) {
			order[o++] = list;	/* DLM first (scratch) */
		}
	}
	for (list = 0; list < partition; list++) {
		if (!bin_map[list].size)
			continue;
		if (strcmp(bin_map[list].name, "ILM") == 0 && o < 8) {
			order[o++] = list;	/* ILM last */
		}
	}

	for (list = 0; list < o; list++) {
		SRAM_addr = bin_map[order[list]].SRAM_addr;
		size = bin_map[order[list]].size;
		BIN_addr = bin_map[order[list]].BIN_addr;
		nvt_crash_append("FWU hw-crc: direct %s BIN=0x%06X SRAM=0x%06X size=0x%05X\n",
				 bin_map[order[list]].name, BIN_addr, SRAM_addr, size);

		if ((BIN_addr + size) > fwsize) {
			NVT_ERR("direct access range (0x%08X to 0x%08X) too big!\n",
				BIN_addr, BIN_addr + size);
			ret = -EINVAL;
			goto direct_out;
		}

		ret = nvt_write_sram(fwdata, SRAM_addr, size + 1, BIN_addr);
		if (ret) {
			NVT_ERR("direct sram program failed, ret = %d\n", ret);
			goto direct_out;
		}
	}

direct_out:
	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen check checksum function.
This function will compare file checksum and fw checksum.

return:
	n.a.
*******************************************************/
static int32_t nvt_check_fw_checksum(void)
{
	uint32_t fw_checksum = 0;
	uint32_t len = partition*4;
	uint32_t list = 0;
	int32_t ret = 0;

	memset(fwbuf, 0, (len+1));

	//---set xdata index to checksum---
	nvt_set_page(ts->mmap->R_ILM_CHECKSUM_ADDR);

	/* read checksum */
	fwbuf[0] = (ts->mmap->R_ILM_CHECKSUM_ADDR) & 0x7F;
	ret = CTP_SPI_READ(ts->client, fwbuf, len+1);
	if (ret) {
		NVT_ERR("Read fw checksum failed\n");
		return ret;
	}

	/*
	 * Compare each checksum from fw
	 * ILM + DLM + Overlay + Info
	 * ilm_dlm_num (ILM & DLM) + ovly_sec_num + info_sec_num
	 */
	for (list = 0; list < partition; list++) {
		fw_checksum = byte_to_word(&fwbuf[1+list*4]);

		/* ignore reserved partition (Reserved Partition size is zero) */
		if(!bin_map[list].size)
			continue;

		if (bin_map[list].crc != fw_checksum) {
			NVT_ERR("[%d] BIN_checksum=0x%08X, FW_checksum=0x%08X\n",
					list, bin_map[list].crc, fw_checksum);
			ret = -EIO;
		}
	}

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen set bootload crc reg bank function.
This function will set hw crc reg before enable crc function.

return:
	n.a.
*******************************************************/
static void nvt_set_bld_crc_bank(uint32_t DES_ADDR, uint32_t SRAM_ADDR,
		uint32_t LENGTH_ADDR, uint32_t size,
		uint32_t G_CHECKSUM_ADDR, uint32_t crc)
{
	/* write destination address */
	nvt_set_page(DES_ADDR);
	fwbuf[0] = DES_ADDR & 0x7F;
	fwbuf[1] = (SRAM_ADDR) & 0xFF;
	fwbuf[2] = (SRAM_ADDR >> 8) & 0xFF;
	fwbuf[3] = (SRAM_ADDR >> 16) & 0xFF;
	CTP_SPI_WRITE(ts->client, fwbuf, 4);

	/* write length */
	nvt_set_page(LENGTH_ADDR);
	fwbuf[0] = LENGTH_ADDR & 0x7F;
	fwbuf[1] = (size) & 0xFF;
	fwbuf[2] = (size >> 8) & 0xFF;
	fwbuf[3] = (size >> 16) & 0xFF;
	if (ts->hw_crc == HWCRC_LEN_2Bytes) {
		CTP_SPI_WRITE(ts->client, fwbuf, 3);
	} else if (ts->hw_crc >= HWCRC_LEN_3Bytes) {
		CTP_SPI_WRITE(ts->client, fwbuf, 4);
	}

	/* write golden dlm checksum */
	nvt_set_page(G_CHECKSUM_ADDR);
	fwbuf[0] = G_CHECKSUM_ADDR & 0x7F;
	fwbuf[1] = (crc) & 0xFF;
	fwbuf[2] = (crc >> 8) & 0xFF;
	fwbuf[3] = (crc >> 16) & 0xFF;
	fwbuf[4] = (crc >> 24) & 0xFF;
	CTP_SPI_WRITE(ts->client, fwbuf, 5);

	return;
}

/*******************************************************
Description:
	Novatek touchscreen set BLD hw crc function.
This function will set ILM and DLM crc information to register.

return:
	n.a.
*******************************************************/
static void nvt_set_bld_hw_crc(void)
{
	/* [0] ILM */
	/* write register bank */

		
	nvt_set_bld_crc_bank(ts->mmap->ILM_DES_ADDR, bin_map[0].SRAM_addr,
			ts->mmap->ILM_LENGTH_ADDR, bin_map[0].size,
			ts->mmap->G_ILM_CHECKSUM_ADDR, bin_map[0].crc);

	/* [1] DLM */
	/* write register bank */
	nvt_set_bld_crc_bank(ts->mmap->DLM_DES_ADDR, bin_map[1].SRAM_addr,
			ts->mmap->DLM_LENGTH_ADDR, bin_map[1].size,
			ts->mmap->G_DLM_CHECKSUM_ADDR, bin_map[1].crc);
}

static void __maybe_unused nvt_dbg_sram_rw_test(void)
{
	uint8_t buf[8] = {0};
	uint32_t addr = 0x38D54; /* history-event scratch, unused by us */
	uint32_t a = 0;

	nvt_set_page(addr);
	buf[0] = addr & 0x7F;
	buf[1] = 0x5A;
	buf[2] = 0x5A;
	buf[3] = 0x5A;
	buf[4] = 0x5A;
	CTP_SPI_WRITE(ts->client, buf, 5);

	memset(buf, 0, sizeof(buf));
	nvt_set_page(addr);
	buf[0] = addr & 0x7F;
	CTP_SPI_READ(ts->client, buf, 5);
	a = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);
	if (a == 0x5A5A5A5A) {
		NVT_ERR("dbg: SRAM WRITE->READ OK @0x%06X (0x%08X)\n", addr, a);
		nvt_crash_append("FWU hw-crc: sram pattern OK\n");
	} else {
		NVT_ERR("dbg: SRAM WRITE->READ FAIL @0x%06X got 0x%08X\n", addr, a);
		nvt_crash_append("FWU hw-crc: sram pattern FAIL 0x%08X\n", a);
	}
}

/*
 * Transport-fidelity probe: push one full NVT_TRANSFER_LEN (31KB) burst
 * into the real DLM SRAM (bin_map[1] scratch, bootloader runs from ILM so
 * this cannot damage it) with a single CTP_SPI_WRITE, then read the whole
 * region back. Logs first-bad offset + total mismatches. If this is clean,
 * the bulk SPI path delivers 31KB bursts losslessly and the fault lives in
 * the chip bootloader/DMA handshake, not the transport.
 */
static void __maybe_unused nvt_dbg_burst_test(void)
{
	uint8_t *rbuf;
	uint32_t sram = 0, i, n, k, bad = 0, first_bad = 0;
	uint32_t wlen;

	rbuf = kmalloc(NVT_READ_LEN + 1, GFP_KERNEL);
	if (!rbuf)
		return;

	for (i = 0; i < partition; i++) {
		if (bin_map[i].size && strcmp(bin_map[i].name, "DLM") == 0) {
			sram = bin_map[i].SRAM_addr;
			break;
		}
	}
	if (!sram) {
		nvt_crash_append("FWU hw-crc: burst test skipped (no DLM target)\n");
		kfree(rbuf);
		return;
	}

	wlen = NVT_TRANSFER_LEN;
	if (wlen > bin_map[1].size)
		wlen = bin_map[1].size;

	for (i = 0; i < wlen; i++)
		fwbuf[1 + i] = (uint8_t)(i & 0xFF);

	nvt_set_page(sram);
	fwbuf[0] = sram & 0x7F;
	if (CTP_SPI_WRITE(ts->client, fwbuf, wlen + 1)) {
		nvt_crash_append("FWU hw-crc: burst SPI write failed\n");
		kfree(rbuf);
		return;
	}

	for (i = 0; i < wlen; i += NVT_READ_LEN) {
		n = min_t(uint32_t, NVT_READ_LEN, wlen - i);
		nvt_set_page(sram + i);
		memset(rbuf, 0, NVT_READ_LEN + 1);
		rbuf[0] = (sram + i) & 0x7F;
		if (CTP_SPI_READ(ts->client, rbuf, n + 1))
			break;
		for (k = 0; k < n; k++) {
			if (rbuf[1 + k] != fwbuf[1 + i + k]) {
				if (!bad)
					first_bad = i + k;
				if (bad < 6)
					nvt_crash_append(
						"FWU hw-crc: burst @0x%06X+0x%05X exp=%02X got=%02X\n",
						sram, i + k, fwbuf[1 + i + k], rbuf[1 + k]);
				bad++;
			}
		}
	}
	kfree(rbuf);

	nvt_crash_append("FWU hw-crc: burst 0x%06X len=0x%X %s bad=%u first_bad=0x%05X\n",
			 sram, wlen, bad ? "MISMATCH" : "OK", bad, first_bad);
	if (!bad)
		nvt_crash_append("FWU hw-crc: DLM burst is bit-exact, transport OK\n");
	NVT_LOG("PROBE burst(31KB) bad=%u\n", bad);
}

/*
 * Transport-fidelity probe, SMALL-window variant.  Same 31KB pattern into the
 * same DLM scratch, but the payload is pushed in NVT_READ_LEN (2KB) chunks
 * with a set_page + offset-byte per chunk -- exactly the cadence the real
 * download uses.  If the 31KB single-write probe shows bad bytes but this one
 * is clean, the SPI transport slips clocks on large single transfers and the
 * firmware download must be chunked smaller.  If BOTH are dirty, the window
 * itself (chip state) is the problem, not transfer size.
 */
static void __maybe_unused nvt_dbg_burst_test_small(void)
{
	uint8_t *rbuf;
	uint32_t sram = 0, i, n, k, bad = 0, first_bad = 0;
	uint32_t wlen;

	rbuf = kmalloc(NVT_READ_LEN + 1, GFP_KERNEL);
	if (!rbuf)
		return;

	for (i = 0; i < partition; i++) {
		if (bin_map[i].size && strcmp(bin_map[i].name, "DLM") == 0) {
			sram = bin_map[i].SRAM_addr;
			break;
		}
	}
	if (!sram) {
		nvt_crash_append("FWU hw-crc: small-burst skipped (no DLM target)\n");
		kfree(rbuf);
		return;
	}

	wlen = NVT_TRANSFER_LEN;
	if (wlen > bin_map[1].size)
		wlen = bin_map[1].size;

	for (i = 0; i < wlen; i++)
		fwbuf[1 + i] = (uint8_t)(i & 0xFF);

	for (i = 0; i < wlen; i += NVT_READ_LEN) {
		n = min_t(uint32_t, NVT_READ_LEN, wlen - i);
		nvt_set_page(sram + i);
		fwbuf[0] = (sram + i) & 0x7F;
		if (CTP_SPI_WRITE(ts->client, fwbuf, n + 1))
			break;
	}

	for (i = 0; i < wlen; i += NVT_READ_LEN) {
		n = min_t(uint32_t, NVT_READ_LEN, wlen - i);
		nvt_set_page(sram + i);
		memset(rbuf, 0, NVT_READ_LEN + 1);
		rbuf[0] = (sram + i) & 0x7F;
		if (CTP_SPI_READ(ts->client, rbuf, n + 1))
			break;
		for (k = 0; k < n; k++) {
			if (rbuf[1 + k] != fwbuf[1 + i + k]) {
				if (!bad)
					first_bad = i + k;
				if (bad < 6)
					nvt_crash_append(
						"FWU hw-crc: small @0x%06X+0x%05X exp=%02X got=%02X\n",
						sram, i + k, fwbuf[1 + i + k], rbuf[1 + k]);
				bad++;
			}
		}
	}
	kfree(rbuf);

	nvt_crash_append("FWU hw-crc: small-burst(2KB xchunks) %s bad=%u first_bad=0x%05X\n",
			 bad ? "MISMATCH" : "OK", bad, first_bad);
	NVT_LOG("PROBE small-burst(2KB x32) bad=%u\n", bad);
}

/* On-demand transport probes, driven from /proc/nvt_fw_control.
 * which: 0 = single 31KB write+readback,  1 = 2KB-chunked write+readback.
 * Safe: writes only to DLM scratch (bootloader runs from ILM).
 * Results reported via NVT_LOG("PROBE ...") so they surface in dmesg. */
void nvt_dbg_probe(int which)
{
	if (!ts || !ts->client)
		return;
	if (which == 0) {
		nvt_dbg_burst_test();
	} else {
		nvt_dbg_burst_test_small();
	}
}

/*
 * Two-consecutive-transaction probe.  Writes the same 31KB pattern into the
 * DLM scratch TWICE at two different pages (tx1 -> screen0, tx2 -> screen1),
 * reading each back right after its own write.  If tx2 is clean but tx1 was
 * clean too, consecutive writes are fine.  If tx2 is dirty (bit-shifted /
 * nibble-folded) while tx1 is clean, the SPI cadence alone is corrupting
 * multi-transfer downloads and a settle delay is the fix.  A second pass adds
 * udelay(10) before tx2 so we can see the cure take effect in the same boot.
 */
static void __maybe_unused nvt_dbg_burst_pair(void)
{
	uint8_t *rbuf;
	uint32_t sram = 0, wlen, i, n, k;
	uint32_t t1_bad = 0, t1_first = 0, t2_bad = 0, t2_first = 0;
	uint32_t off2 = 0x8000; /* second page inside DLM */

	rbuf = kmalloc(NVT_READ_LEN + 1, GFP_KERNEL);
	if (!rbuf)
		return;

	for (i = 0; i < partition; i++) {
		if (bin_map[i].size && strcmp(bin_map[i].name, "DLM") == 0) {
			sram = bin_map[i].SRAM_addr;
			break;
		}
	}
	if (!sram) {
		nvt_crash_append("FWU hw-crc: pair probe skipped (no DLM target)\n");
		kfree(rbuf);
		return;
	}

	wlen = NVT_TRANSFER_LEN;
	if (wlen > bin_map[1].size)
		wlen = bin_map[1].size;

	for (i = 0; i < wlen; i++)
		fwbuf[1 + i] = (uint8_t)(i & 0xFF);

	/* pass 1: back-to-back big writes, no delay */
	nvt_set_page(sram);
	fwbuf[0] = sram & 0x7F;
	if (CTP_SPI_WRITE(ts->client, fwbuf, wlen + 1))
		goto out_free;

	nvt_set_page(sram + off2);
	fwbuf[0] = (sram + off2) & 0x7F;
	if (CTP_SPI_WRITE(ts->client, fwbuf, wlen + 1))
		goto out_free;

	/* readback tx1 (cleanest region) */
	for (i = 0; i < wlen; i += NVT_READ_LEN) {
		n = min_t(uint32_t, NVT_READ_LEN, wlen - i);
		nvt_set_page(sram + i);
		memset(rbuf, 0, NVT_READ_LEN + 1);
		rbuf[0] = (sram + i) & 0x7F;
		if (CTP_SPI_READ(ts->client, rbuf, n + 1))
			break;
		for (k = 0; k < n; k++) {
			if (rbuf[1 + k] != fwbuf[1 + i + k]) {
				if (!t1_bad)
					t1_first = i + k;
				t1_bad++;
			}
		}
	}

	/* readback tx2 */
	for (i = 0; i < wlen; i += NVT_READ_LEN) {
		n = min_t(uint32_t, NVT_READ_LEN, wlen - i);
		nvt_set_page(sram + off2 + i);
		memset(rbuf, 0, NVT_READ_LEN + 1);
		rbuf[0] = (sram + off2 + i) & 0x7F;
		if (CTP_SPI_READ(ts->client, rbuf, n + 1))
			break;
		for (k = 0; k < n; k++) {
			if (rbuf[1 + k] != fwbuf[1 + i + k]) {
				if (!t2_bad)
					t2_first = i + k;
				t2_bad++;
			}
		}
	}

	nvt_crash_append("FWU hw-crc: pair no-delay tx1 bad=%u f=%06X tx2 bad=%u f=%06X\n",
			 t1_bad, sram + t1_first, t2_bad, sram + off2 + t2_first);
	NVT_LOG("PROBE pair no-delay tx1 bad=%u tx2 bad=%u\n", t1_bad, t2_bad);

	/* pass 2: same, but udelay(10) before tx2 */
	t1_bad = t1_first = t2_bad = t2_first = 0;

	nvt_set_page(sram);
	fwbuf[0] = sram & 0x7F;
	if (CTP_SPI_WRITE(ts->client, fwbuf, wlen + 1))
		goto out_free;

	udelay(10);

	nvt_set_page(sram + off2);
	fwbuf[0] = (sram + off2) & 0x7F;
	if (CTP_SPI_WRITE(ts->client, fwbuf, wlen + 1))
		goto out_free;

	for (i = 0; i < wlen; i += NVT_READ_LEN) {
		n = min_t(uint32_t, NVT_READ_LEN, wlen - i);
		nvt_set_page(sram + off2 + i);
		memset(rbuf, 0, NVT_READ_LEN + 1);
		rbuf[0] = (sram + off2 + i) & 0x7F;
		if (CTP_SPI_READ(ts->client, rbuf, n + 1))
			break;
		for (k = 0; k < n; k++) {
			if (rbuf[1 + k] != fwbuf[1 + i + k]) {
				if (!t2_bad)
					t2_first = i + k;
				t2_bad++;
			}
		}
	}
	nvt_crash_append("FWU hw-crc: pair delay10 tx2 bad=%u f=%06X\n",
			 t2_bad, sram + off2 + t2_first);
	NVT_LOG("PROBE pair delay10 tx2 bad=%u\n", t2_bad);

out_free:
	kfree(rbuf);
}

static void __maybe_unused nvt_dbg_readback_verify(void)
{
	uint8_t buf[16] = {0};
	uint32_t list = 0;
	uint32_t offs[3];
	int32_t s = 0;

	for (list = 0; list < partition; list++) {
		uint32_t matched = 0, bad = 0;
		uint32_t b, a, n, i;

		if (!bin_map[list].size)
			continue;

		offs[0] = 0;
		offs[1] = bin_map[list].size / 2;
		offs[2] = (bin_map[list].size > 8) ? (bin_map[list].size - 8) : 0;

		for (s = 0; s < 3; s++) {
			a = bin_map[list].SRAM_addr + offs[s];
			b = bin_map[list].BIN_addr + offs[s];
			n = (bin_map[list].size - offs[s] > 8) ? 8 : (bin_map[list].size - offs[s]);

			memset(buf, 0, sizeof(buf));
			nvt_set_page(a);
			buf[0] = a & 0x7F;
			if (CTP_SPI_READ(ts->client, buf, n + 1))
				goto out_part;

			for (i = 0; i < n; i++)
				if (buf[1 + i] != fw_entry->data[b + i])
					break;

			nvt_crash_append("FWU rbk %s+0x%06X file=%02X%02X%02X%02X chip=%02X%02X%02X%02X %s\n",
					 bin_map[list].name, offs[s],
					 fw_entry->data[b], fw_entry->data[b+1],
					 fw_entry->data[b+2], fw_entry->data[b+3],
					 buf[1], buf[2], buf[3], buf[4],
					 (i == n) ? "OK" : "MISMATCH");
			if (i == n)
				matched++;
			else
				bad++;
		}
out_part:
		nvt_crash_append("FWU rbk %s total: %u match %u bad\n",
				 bin_map[list].name, matched + bad, matched, bad);
	}
}

static void __maybe_unused nvt_dbg_read_crc_bank(void)
{
	uint8_t buf[8] = {0};
	uint32_t a = 0;

	/* read ILM CRC registers */
	nvt_set_page(ts->mmap->G_ILM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_ILM_CHECKSUM_ADDR & 0x7F;
	buf[1] = buf[2] = buf[3] = buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	a = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);
	NVT_ERR("dbg: G_ILM(0x%06X) readback = 0x%08X\n",
		ts->mmap->G_ILM_CHECKSUM_ADDR, a);

	nvt_set_page(ts->mmap->G_DLM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_DLM_CHECKSUM_ADDR & 0x7F;
	buf[1] = buf[2] = buf[3] = buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	a = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);
	NVT_ERR("dbg: G_DLM(0x%06X) readback = 0x%08X\n",
		ts->mmap->G_DLM_CHECKSUM_ADDR, a);

	nvt_set_page(ts->mmap->ILM_DES_ADDR);
	buf[0] = ts->mmap->ILM_DES_ADDR & 0x7F;
	buf[1] = buf[2] = buf[3] = 0x00;
	CTP_SPI_READ(ts->client, buf, 4);
	NVT_ERR("dbg: ILM_DES(0x%06X) readback = 0x%06X\n",
		ts->mmap->ILM_DES_ADDR,
		(buf[1] | (buf[2] << 8) | (buf[3] << 16)) & 0xFFFFFF);

	nvt_set_page(ts->mmap->DLM_DES_ADDR);
	buf[0] = ts->mmap->DLM_DES_ADDR & 0x7F;
	buf[1] = buf[2] = buf[3] = 0x00;
	CTP_SPI_READ(ts->client, buf, 4);
	NVT_ERR("dbg: DLM_DES(0x%06X) readback = 0x%06X\n",
		ts->mmap->DLM_DES_ADDR,
		(buf[1] | (buf[2] << 8) | (buf[3] << 16)) & 0xFFFFFF);

	/* --- write->readback self-test ---
	 * Prove the CRC bank is writable at all: write a known pattern to
	 * G_DLM_CHECKSUM and read it back. If it does not persist, the
	 * bootloader is not accepting host bank programming and none of the
	 * download setup can work no matter the ordering.
	 */
	nvt_set_page(ts->mmap->G_DLM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_DLM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0xEF;
	buf[2] = 0xBE;
	buf[3] = 0xAD;
	buf[4] = 0xDE;
	CTP_SPI_WRITE(ts->client, buf, 5);

	memset(buf, 0, sizeof(buf));
	nvt_set_page(ts->mmap->G_DLM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_DLM_CHECKSUM_ADDR & 0x7F;
	CTP_SPI_READ(ts->client, buf, 5);
	a = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);
	if (a == 0xDEADBEEF) {
		NVT_ERR("dbg: CRC bank WRITE->READ OK (pattern 0x%08X)\n", a);
		nvt_crash_append("FWU hw-crc: crc-bank pattern OK\n");
	} else {
		NVT_ERR("dbg: CRC bank WRITE->READ FAIL (got 0x%08X)\n", a);
		nvt_crash_append("FWU hw-crc: crc-bank pattern FAIL 0x%08X\n", a);
	}

	/* restore real DLM golden crc overwritten by the pattern */
	if (bin_map) {
		nvt_set_page(ts->mmap->G_DLM_CHECKSUM_ADDR);
		buf[0] = ts->mmap->G_DLM_CHECKSUM_ADDR & 0x7F;
		buf[1] = (bin_map[1].crc) & 0xFF;
		buf[2] = (bin_map[1].crc >> 8) & 0xFF;
		buf[3] = (bin_map[1].crc >> 16) & 0xFF;
		buf[4] = (bin_map[1].crc >> 24) & 0xFF;
		CTP_SPI_WRITE(ts->client, buf, 5);
	}
}

/*******************************************************
Description:
	Novatek touchscreen read BLD hw crc info function.
This function will check crc results from register.

return:
	n.a.
*******************************************************/
static void nvt_read_bld_hw_crc(void)
{
	uint8_t buf[8] = {0};
	uint32_t g_crc = 0, r_crc = 0;

	/* CRC Flag */
	nvt_set_page(ts->mmap->BLD_ILM_DLM_CRC_ADDR);
	buf[0] = ts->mmap->BLD_ILM_DLM_CRC_ADDR & 0x7F;
	buf[1] = 0x00;
	CTP_SPI_READ(ts->client, buf, 2);
	NVT_ERR("crc_done = %d, ilm_crc_flag = %d, dlm_crc_flag = %d\n",
			(buf[1] >> 2) & 0x01, (buf[1] >> 0) & 0x01, (buf[1] >> 1) & 0x01);
	nvt_crash_append("FWU hw-crc: bld crc flag=0x%02X\n", buf[1]);

	/* ILM CRC */
	nvt_set_page(ts->mmap->G_ILM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_ILM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	g_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	nvt_set_page(ts->mmap->R_ILM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->R_ILM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	r_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	NVT_ERR("ilm: bin crc = 0x%08X, golden = 0x%08X, result = 0x%08X\n",
			bin_map[0].crc, g_crc, r_crc);
	nvt_crash_append("FWU hw-crc: ilm golden=0x%08X result=0x%08X\n",
			 g_crc, r_crc);

	/* DLM CRC */
	nvt_set_page(ts->mmap->G_DLM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->G_DLM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	g_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	nvt_set_page(ts->mmap->R_DLM_CHECKSUM_ADDR);
	buf[0] = ts->mmap->R_DLM_CHECKSUM_ADDR & 0x7F;
	buf[1] = 0x00;
	buf[2] = 0x00;
	buf[3] = 0x00;
	buf[4] = 0x00;
	CTP_SPI_READ(ts->client, buf, 5);
	r_crc = buf[1] | (buf[2] << 8) | (buf[3] << 16) | (buf[4] << 24);

	NVT_ERR("dlm: bin crc = 0x%08X, golden = 0x%08X, result = 0x%08X\n",
			bin_map[1].crc, g_crc, r_crc);
	nvt_crash_append("FWU hw-crc: dlm golden=0x%08X result=0x%08X\n",
			 g_crc, r_crc);

	return;
}

static void __maybe_unused nvt_spi_dma_setup(void)
{
	uint8_t buf[33] = {0};

	if (ts->mmap->SPI_DMA_VAL_ADDR) {
		nvt_set_page(bin_map[1].SRAM_addr);
		buf[0] = bin_map[1].SRAM_addr & 0x7F;
		CTP_SPI_WRITE(ts->client, buf, 33);

		nvt_set_page(ts->mmap->SPI_DMA_VAL_ADDR);
		buf[0] = ts->mmap->SPI_DMA_VAL_ADDR & 0x7F;
		buf[1] = 0x35;
		buf[2] = 0x32;
		buf[3] = 0xAA;
		buf[4] = 0x00;
		CTP_SPI_WRITE(ts->client, buf, 5);

		NVT_LOG("set spi dma val finish\n");
	} else {
		NVT_ERR("spi dma val addr is NULL\n");
	}
}

/*******************************************************
Description:
	Novatek touchscreen Download_Firmware with HW CRC
function. It's complete download firmware flow.

return:
	Executive outcomes. 0---succeed. else---fail.
*******************************************************/
static int32_t nvt_download_firmware_hw_crc(void)
{
	uint8_t retry = 0;
	int32_t ret = 0;

	start = ktime_get();

	while (1) {
		NVT_LOG("retry=%d, bootloader_reset...\n", retry);
		nvt_crash_append("FWU hw-crc: bootloader_reset\n");

		/* bootloader reset to reset MCU */
		nvt_bootloader_reset();

		/* Give the chip's bootloader time to initialize its SPI-DMA engine.
		 * Without this delay, wait_auto_copy() polls 0xFF forever (the
		 * auto-copy mode never arms) and the download times out.  Android
		 * gets this time implicitly because the chip sits in bootloader
		 * for seconds between probe and download; our immediate flow
		 * must wait explicitly. */
		msleep(200);

		/* set ilm & dlm reg bank */
		nvt_set_bld_hw_crc();
		nvt_crash_append("FWU hw-crc: crc bank set\n");

		/* Start to write firmware process */
		if (cascade_2nd_header_info) {
			/* Android DMA-auto-copy sequence.  No debug reads/writes in the
			 * critical path (previous probes perturbed the register window
			 * and broke the download). */
			nvt_spi_dma_setup();
			nvt_crash_append("FWU hw-crc: spi_dma_setup armed\n");

			nvt_tx_auto_copy_mode();
			nvt_crash_append("FWU hw-crc: tx_auto_copy_mode\n");

			ret = nvt_write_firmware(fw_entry->data, fw_entry->size);
			nvt_crash_append("FWU hw-crc: write_firmware ret=%d\n", ret);
			if (ret) {
				NVT_ERR("Write_Firmware failed. (%d)\n", ret);
				goto fail;
			}

			ret = nvt_wait_auto_copy();
			nvt_crash_append("FWU hw-crc: wait_auto_copy ret=%d\n", ret);
			if (ret) {
				NVT_ERR("wait auto copy failed. (%d)\n", ret);
				goto fail;
			}
		} else {
			nvt_crash_append("FWU hw-crc: writing fw without auto_copy (%u B)\n",
					 fw_entry->size);
			ret = nvt_write_firmware(fw_entry->data, fw_entry->size);
			nvt_crash_append("FWU hw-crc: write_firmware ret=%d\n", ret);
			if (ret) {
				NVT_ERR("Write_Firmware failed. (%d)\n", ret);
				goto fail;
			}
		}

		/* clear fw reset status & enable fw crc check */
		NVT_LOG("fw_crc_enable...\n");
		nvt_crash_append("FWU hw-crc: fw_crc_enable\n");
		nvt_fw_crc_enable();

		/* Set Boot Ready Bit */
		NVT_LOG("boot_ready...\n");
		nvt_crash_append("FWU hw-crc: boot_ready\n");
		nvt_boot_ready();

		NVT_LOG("check_fw_reset_state...\n");
		nvt_crash_append("FWU hw-crc: check_fw_reset_state\n");
		ret = nvt_check_fw_reset_state(RESET_STATE_INIT);
		if (ret)
			goto fail;
		break;

	fail:
		retry++;
		if(unlikely(retry > 2)) {
			NVT_ERR("error, retry=%d\n", retry);
			nvt_read_bld_hw_crc();
			/* C4 diagnostic: read back what actually landed in SRAM after the
			 * failed download.  Runs only post-failure so it cannot perturb
			 * the auto-copy.  Tells us whether corruption is systematic
			 * (shift/order/page-window) or random for a given partition. */
			nvt_verify_sram(fw_entry->data, fw_entry->size);
			break;
		}
	}

	end = ktime_get();

	/* Always restore page to EVENT_BUF_ADDR so touch event reading works */
	nvt_set_page(ts->mmap->EVENT_BUF_ADDR);

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen Download_Firmware function. It's
complete download firmware flow.

return:
	n.a.
*******************************************************/
static int32_t nvt_download_firmware(void)
{
	uint8_t retry = 0;
	int32_t ret = 0;

	start = ktime_get();

	while (1) {
		/*
		 * Send eng reset cmd before download FW
		 * Keep TP_RESX low when send eng reset cmd
		 */
#if NVT_TOUCH_SUPPORT_HW_RST
		gpio_set_value(ts->reset_gpio, 0);
		mdelay(1);	//wait 1ms
#endif
		nvt_eng_reset();
#if NVT_TOUCH_SUPPORT_HW_RST
		gpio_set_value(ts->reset_gpio, 1);
		mdelay(10);	//wait tRT2BRST after TP_RST
#endif
		nvt_bootloader_reset();

		/* clear fw reset status */
		nvt_write_addr(ts->mmap->EVENT_BUF_ADDR | EVENT_MAP_RESET_COMPLETE, 0x00);

		/* Start to write firmware process */
		ret = nvt_write_firmware(fw_entry->data, fw_entry->size);
		if (ret) {
			NVT_ERR("Write_Firmware failed. (%d)\n", ret);
			goto fail;
		}

		/* Set Boot Ready Bit */
		nvt_boot_ready();

		ret = nvt_check_fw_reset_state(RESET_STATE_INIT);
		if (ret) {
			NVT_ERR("nvt_check_fw_reset_state failed. (%d)\n", ret);
			goto fail;
		}

		/* check fw checksum result */
		ret = nvt_check_fw_checksum();
		if (ret) {
			NVT_ERR("firmware checksum not match, retry=%d\n", retry);
			goto fail;
		} else {
			break;
		}

fail:
		retry++;
		if(unlikely(retry > 2)) {
			NVT_ERR("error, retry=%d\n", retry);
			break;
		}
	}

	end = ktime_get();

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen update firmware main function.

return:
	n.a.
*******************************************************/
int32_t nvt_update_firmware(char *firmware_name)
{
	int32_t ret = 0;

	// request bin file in "/etc/firmware"
	ret = update_firmware_request(firmware_name);
	nvt_crash_append("FWU stage1: fw request ret=%d\n", ret);
	if (ret) {
		NVT_ERR("update_firmware_request failed. (%d)\n", ret);
		goto request_firmware_fail;
	}

	/* initial buffer and variable */
	ret = nvt_download_init();
	nvt_crash_append("FWU stage2: download_init ret=%d\n", ret);
	if (ret) {
		NVT_ERR("Download Init failed. (%d)\n", ret);
		goto download_fail;
	}

	/* download firmware process */
	nvt_crash_append("FWU stage3: starting download (hw_crc=%d)\n", ts->hw_crc);
	if (ts->hw_crc)
		ret = nvt_download_firmware_hw_crc();
	else
		ret = nvt_download_firmware();
	nvt_crash_append("FWU stage4: download returned ret=%d\n", ret);
	if (ret) {
		NVT_ERR("Download Firmware failed. (%d)\n", ret);
		goto download_fail;
	}

	NVT_LOG("Update firmware success! <%ld us>\n",
			(long) ktime_us_delta(end, start));

	/* Get FW Info */
	ret = nvt_get_fw_info();
	nvt_crash_append("FWU stage5: get_fw_info after dl ret=%d ver=0x%02X\n",
			 ret, ts->fw_ver);
	if (ret) {
		NVT_ERR("nvt_get_fw_info failed. (%d)\n", ret);
	}

download_fail:
	if (!IS_ERR_OR_NULL(bin_map)) {
		kfree(bin_map);
		bin_map = NULL;
	}

	update_firmware_release();
request_firmware_fail:

	return ret;
}

/*******************************************************
Description:
	Novatek touchscreen update firmware when booting
	function.

return:
	n.a.
*******************************************************/
void Boot_Update_Firmware(struct work_struct *work)
{
	int32_t ret = 0;
	int attempt = 0;
	bool app_ok = false;

	nvt_crash_append("FWU begin (deferred boot-time update)\n");
	/*
	 * Mirror Android's proven NT36523N boot flow verbatim:
	 *   1. Park the chip in the bootloader FIRST (nvt_bootloader_reset),
	 *      exactly as Android's probe does before every trim read. If we
	 *      read trim without parking, the TDDI has already auto-booted its
	 *      internal (read-only) firmware and we read unstable/garbage trim,
	 *      and subsequent host writes to the CRC bank / SRAM are rejected.
	 *   2. identify chip from SPI trim so the poll base / hw-crc mmap is right
	 *   3. eng_reset puts the MCU in a deterministic state, bootloader_reset
	 *      keeps it IN the bootloader (Android probe does exactly this and
	 *      leaves the chip in bootloader; the +7s update then downloads
	 *      straight from bootloader -- no sw_reset, no app bounce in between)
	 *   4. download via HW-CRC path (cascade / SPI-DMA auto-copy)
	 *   5. after the chip re-boots to the app, require a REAL fwinfo read:
	 *      version must equal the bin version (0x19) and PID must match,
	 *      otherwise the download did not commit and we retry.
	 *
	 * DECISIVE (2026-08-31, slot B md5 e648c7d8): after the probe boots the
	 * chip exactly like Android (trim loop ret=0, reset=0x1A), the natural
	 * un-downloaded fwinfo reads fw_ver=0x20,~fw_ver=0x0F (fails complement,
	 * NOT 0x19/0xE6). So the chip's internal flash does NOT hold the touch
	 * app -- Android's 0x19 comes ONLY from its every-boot download. The
	 * download path is MANDATORY and is the sole blocker. Run the real
	 * Android-mirror download below now that the probe parked the chip in
	 * the bootloader correctly.
	 */
	nvt_crash_append("FWU proceeding with mandatory download\n");
	nvt_bootloader_reset();
	msleep(10);
	nvt_identify_chip();

	mutex_lock(&ts->lock);

	nvt_eng_reset();
	msleep(10);
	nvt_bootloader_reset();

	while (attempt < 3) {
		nvt_crash_append("FWU attempt %d: download\n", attempt);
		ret = nvt_update_firmware(boot_update_firmware_name);
		nvt_crash_append("FWU nvt_update_firmware ret=%d\n", ret);
		if (ret) {
			attempt++;
			continue;
		}

		/*
		 * nvt_update_firmware() culminates in boot_ready -> the MCU resets
		 * and runs the just-staged app. get_fw_info() now reads the LIVE
		 * app's fwinfo region; only a committed download yields 0x19/PID.
		 */
		ret = nvt_get_fw_info();
		nvt_crash_append("FWU post-dl get_fw_info ret=%d ver=0x%02X pid=0x%04X\n",
				 ret, ts->fw_ver, ts->nvt_pid);
		if (ret == 0 && ts->fw_ver == 0x19 && ts->nvt_pid == 0x6073) {
			app_ok = true;
			break;
		}
		attempt++;
	}

	if (!app_ok && nvt_panic_on_fail)
		nvt_dump_and_panic("nvt: boot fw update failed");

	mutex_unlock(&ts->lock);

	/*
	 * On success the chip is already running the app from boot_ready -- do
	 * not bounce it again (Android does not). On failure, issue one soft
	 * reset so the flash auto-copy can at least boot the stock app.
	 */
	if (!app_ok) {
		nvt_sw_reset();
		msleep(100);
	}
	nvt_poll_start(ts);
	msleep(20);
	nvt_force_continuous_scan();
#if NVT_SUPPORT_PEN
	if(ts->probe_state == 1){
		nvt_support_pen_set(0);
		ts->probe_state = 0;
	}
#endif
#if NVT_CUST_PROC_CMD
	queue_work(nvt_fwu_wq, &ts_restore_cmd_work);
#endif
}
#endif /* BOOT_UPDATE_FIRMWARE */
