/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(net_otPlat_settings, CONFIG_OPENTHREAD_L2_LOG_LEVEL);

#include <kernel.h>
#include <device.h>
#include <fs.h>
#include <nffs/nffs.h>

#include <openthread/platform/settings.h>

#define MAX_SETTING_PATH_LEN 32
#define MOUNT_POINT "/nffs"
#define OT_SETTINGS_DIR "/nffs/ot"

/* NFFS work area struct */
static struct nffs_flash_desc flash_desc;

/* Mounting info */
static struct fs_mount_t nffs_mnt = {
	.type = FS_NFFS,
	.mnt_point = MOUNT_POINT,
	.fs_data = &flash_desc,
};

static void get_setting_dir_path(char *buf, size_t buflen, u16_t key)
{
	int ret;

	ret = snprintf(buf, buflen, "%s/%u", OT_SETTINGS_DIR, key);
	__ASSERT(ret < buflen, "Maximum setting path to small");
}

static void generate_setting_file_path(char *buf, size_t buflen, u16_t key)
{
	int ret;
	struct fs_dirent entry;

	do {
		ret = snprintf(buf, buflen, "%s/%u/%08x",
			       OT_SETTINGS_DIR, key, sys_rand32_get());
		__ASSERT(ret < buflen, "Maximum setting path to small");
	} while (fs_stat(buf, &entry) == 0);
}

static void get_setting_file_path(char *buf, size_t buflen, u16_t key,
				  char *filename)
{
	int ret;

	ret = snprintf(buf, buflen, "%s/%u/%s", OT_SETTINGS_DIR, key, filename);
	__ASSERT(ret < buflen, "Maximum setting path to small");
}

static void get_setting_file_entry(char *buf, size_t buflen, u16_t key,
				   int index, struct fs_dirent *entry)
{
	bool found = false;
	int index_cnt = 0;
	int ret;
	struct fs_dir_t dir;

	get_setting_dir_path(buf, buflen, key);

	ret = fs_opendir(&dir, buf);
	if (ret != 0) {
		LOG_ERR("Setting id: %u not found", key);
		goto exit;
	}

	while (1) {
		ret = fs_readdir(&dir, entry);
		if (ret != 0 || entry->name[0] == '\0') {
			LOG_ERR("Setting id: %u index: %u not found",
				key, index);
			break;
		}

		/* It should not happen that we store other directories within,
		 * setting directory, but ignore them just in case.
		 */
		if (entry->type != FS_DIR_ENTRY_FILE) {
			continue;
		}

		if (index == index_cnt) {
			found = true;
			break;
		}

		index_cnt++;
	}

	ret = fs_closedir(&dir);
	__ASSERT(ret == 0, "Failed to close a directory %d.", ret);

exit:
	if (!found) {
		entry->name[0] = '\0';
	}

	return;
}

static otError setting_add(char *buf, size_t buflen, u16_t key,
			   const u8_t *value, u16_t vallen)
{
	struct fs_file_t file;
	int ret;

	get_setting_dir_path(buf, buflen, key);

	ret = fs_mkdir(buf);
	__ASSERT(ret == 0 || ret == -EEXIST,
		 "Failed to create setting directory %d", ret);

	generate_setting_file_path(buf, buflen, key);

	ret = fs_open(&file, buf);
	__ASSERT(ret == 0 , "Failed to create setting file %d", ret);

	ret = fs_write(&file, value, vallen);
	__ASSERT(ret == vallen, "Failed to write a file.");

	ret = fs_close(&file);
	__ASSERT(ret == 0, "Failed to close a file.");

	return OT_ERROR_NONE;
}

void otPlatSettingsInit(otInstance *aInstance)
{
	ARG_UNUSED(aInstance);

	struct device *flash_dev;
	int ret;

	flash_dev = device_get_binding("NRF_FLASH_DRV_NAME");
	__ASSERT(flash_dev, "No flash device find");

	nffs_mnt.storage_dev = flash_dev;

	ret = fs_mount(&nffs_mnt);
	__ASSERT(ret == 0, "Error mounting filesystem %d", ret);

	ret = fs_mkdir(OT_SETTINGS_DIR);
	__ASSERT(ret == 0 || ret == -EEXIST,
		 "Failed to create OT directory %d", ret);
}

otError otPlatSettingsBeginChange(otInstance *aInstance)
{
	ARG_UNUSED(aInstance);

	/* TODO Implementation. */

	return OT_ERROR_NONE;
}

otError otPlatSettingsCommitChange(otInstance *aInstance)
{
	ARG_UNUSED(aInstance);

	/* TODO Implementation. */

	return OT_ERROR_NONE;
}

otError otPlatSettingsAbandonChange(otInstance *aInstance)
{
	ARG_UNUSED(aInstance);

	/* TODO Implementation. */

	return OT_ERROR_NONE;
}

otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
	ARG_UNUSED(aInstance);

	LOG_INF("otPlatSettingsGet Entry aKey %u aIndex %d", aKey, aIndex);

	int ret;
	size_t read_len;
	char setting_path[MAX_SETTING_PATH_LEN];
	struct fs_dirent entry;
	struct fs_file_t file;

	get_setting_file_entry(setting_path, sizeof(setting_path), aKey, aIndex, &entry);
	if (entry.name[0] == '\0') {
		return OT_ERROR_NOT_FOUND;
	}

	if (aValueLength == NULL) {
		/* Presence check */
		return OT_ERROR_NONE;
	}

	if (aValue == NULL) {
		/* Length check */
		*aValueLength = entry.size;
		return OT_ERROR_NONE;
	}

	read_len = entry.size;

	if (*aValueLength < entry.size) {
		/* If the setting won't fit, copy as much as we can. */
		read_len = *aValueLength;
	}

	get_setting_file_path(setting_path, sizeof(setting_path), aKey,
			      entry.name);

	/* Copy the actual setting. */
	ret = fs_open(&file, setting_path);
	__ASSERT(ret == 0 , "Failed to open setting file %d", ret);

	ret = fs_read(&file, aValue, read_len);
	__ASSERT(ret == read_len, "Failed to read a file.");

	ret = fs_close(&file);
	__ASSERT(ret == 0, "Failed to close a file %d.", ret);

	*aValueLength = entry.size;

	return OT_ERROR_NONE;
}

otError otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
	ARG_UNUSED(aInstance);

	LOG_INF("otPlatSettingsSet Entry aKey %u", aKey);

	char setting_path[MAX_SETTING_PATH_LEN];
	int ret;

	get_setting_dir_path(setting_path, sizeof(setting_path), aKey);

	ret = fs_unlink(setting_path);
	__ASSERT(ret == 0 || ret == -ENOENT,
		 "Failed to unlink directory, err = %d", ret);

	return setting_add(setting_path, sizeof(setting_path), aKey, aValue, aValueLength);
}

otError otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
	ARG_UNUSED(aInstance);

	LOG_INF("otPlatSettingsAdd Entry aKey %u", aKey);

	char setting_path[MAX_SETTING_PATH_LEN];

	return setting_add(setting_path, sizeof(setting_path), aKey, aValue, aValueLength);
}

otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
	ARG_UNUSED(aInstance);

	LOG_INF("otPlatSettingsDelete Entry aKey %u aIndex %d", aKey, aIndex);

	int ret;
	char setting_path[MAX_SETTING_PATH_LEN];
	struct fs_dirent entry;

	if (aIndex == -1) {
		/* Remove entire setting directory. */
		get_setting_dir_path(setting_path, sizeof(setting_path), aKey);
	} else {
		/* Remove specific setting file. */
		get_setting_file_entry(setting_path, sizeof(setting_path),
				       aKey, aIndex, &entry);
		if (entry.name[0] == '\0') {
			return OT_ERROR_NOT_FOUND;
		}

		get_setting_file_path(setting_path, sizeof(setting_path), aKey,
				      entry.name);
	}

	ret = fs_unlink(setting_path);
	if (ret == -ENOENT) {
		return OT_ERROR_NOT_FOUND;
	}
	__ASSERT(ret == 0 , "Failed to unlink setting file %d", ret);

	return OT_ERROR_NONE;
}

void otPlatSettingsWipe(otInstance *aInstance)
{
	ARG_UNUSED(aInstance);

	int ret;

	LOG_INF("otPlatSettingsWipe Entry");

	ret = fs_unlink(OT_SETTINGS_DIR);
	__ASSERT(ret == 0, "Failed to unlink directory, err = %d", ret);

	ret = fs_mkdir(OT_SETTINGS_DIR);
	__ASSERT(ret == 0 || ret == -EEXIST,
		 "Failed to create OT directory %d", ret);
}
