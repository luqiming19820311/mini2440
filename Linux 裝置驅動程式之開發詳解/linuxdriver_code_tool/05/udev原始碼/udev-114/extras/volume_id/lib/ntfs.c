/*
 * volume_id - reads filesystem label and uuid
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation version 2 of the License.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "libvolume_id.h"
#include "util.h"

static struct ntfs_super_block {
    uint8_t     jump[3];
    uint8_t     oem_id[8];
    uint16_t    bytes_per_sector;
    uint8_t     sectors_per_cluster;
    uint16_t    reserved_sectors;
    uint8_t     fats;
    uint16_t    root_entries;
    uint16_t    sectors;
    uint8_t     media_type;
    uint16_t    sectors_per_fat;
    uint16_t    sectors_per_track;
    uint16_t    heads;
    uint32_t    hidden_sectors;
    uint32_t    large_sectors;
    uint16_t    unused[2];
    uint64_t    number_of_sectors;
    uint64_t    mft_cluster_location;
    uint64_t    mft_mirror_cluster_location;
    int8_t      cluster_per_mft_record;
    uint8_t     reserved1[3];
    int8_t      cluster_per_index_record;
    uint8_t     reserved2[3];
    uint8_t     volume_serial[8];
    uint16_t    checksum;
} PACKED* ns;

static struct master_file_table_record {
    uint8_t     magic[4];
    uint16_t    usa_ofs;
    uint16_t    usa_count;
    uint64_t    lsn;
    uint16_t    sequence_number;
    uint16_t    link_count;
    uint16_t    attrs_offset;
    uint16_t    flags;
    uint32_t    bytes_in_use;
    uint32_t    bytes_allocated;
} PACKED* mftr;

static struct file_attribute {
    uint32_t    type;
    uint32_t    len;
    uint8_t     non_resident;
    uint8_t     name_len;
    uint16_t    name_offset;
    uint16_t    flags;
    uint16_t    instance;
    uint32_t    value_len;
    uint16_t    value_offset;
} PACKED* attr;

static struct volume_info {
    uint64_t    reserved;
    uint8_t     major_ver;
    uint8_t     minor_ver;
} PACKED* info;

#define MFT_RECORD_VOLUME           3
#define MFT_RECORD_ATTR_VOLUME_NAME     0x60
#define MFT_RECORD_ATTR_VOLUME_INFO     0x70
#define MFT_RECORD_ATTR_OBJECT_ID       0x40
#define MFT_RECORD_ATTR_END         0xffffffffu

int volume_id_probe_ntfs(struct volume_id* id, uint64_t off, uint64_t size) {
    unsigned int sector_size;
    unsigned int cluster_size;
    uint64_t mft_cluster;
    uint64_t mft_off;
    unsigned int mft_record_size;
    unsigned int attr_type;
    unsigned int attr_off;
    unsigned int attr_len;
    unsigned int val_off;
    unsigned int val_len;
    const uint8_t* buf;
    const uint8_t* val;

    info("probing at offset 0x%llx", (unsigned long long) off);

    ns = (struct ntfs_super_block*) volume_id_get_buffer(id, off, 0x200);

    if (ns == NULL) {
        return -1;
    }

    if (memcmp(ns->oem_id, "NTFS", 4) != 0) {
        return -1;
    }

    volume_id_set_uuid(id, ns->volume_serial, 0, UUID_64BIT_LE);

    sector_size = le16_to_cpu(ns->bytes_per_sector);
    cluster_size = ns->sectors_per_cluster * sector_size;
    mft_cluster = le64_to_cpu(ns->mft_cluster_location);
    mft_off = mft_cluster * cluster_size;

    if (ns->cluster_per_mft_record < 0)
        /* size = -log2(mft_record_size); normally 1024 Bytes */
    {
        mft_record_size = 1 << -ns->cluster_per_mft_record;
    } else {
        mft_record_size = ns->cluster_per_mft_record * cluster_size;
    }

    dbg("sectorsize  0x%x", sector_size);
    dbg("clustersize 0x%x", cluster_size);
    dbg("mftcluster  %llu", (unsigned long long) mft_cluster);
    dbg("mftoffset  0x%llx", (unsigned long long) mft_off);
    dbg("cluster per mft_record  %i", ns->cluster_per_mft_record);
    dbg("mft record size  %i", mft_record_size);

    buf = volume_id_get_buffer(id, off + mft_off + (MFT_RECORD_VOLUME * mft_record_size),
                               mft_record_size);

    if (buf == NULL) {
        goto found;
    }

    mftr = (struct master_file_table_record*) buf;

    dbg("mftr->magic '%c%c%c%c'", mftr->magic[0], mftr->magic[1], mftr->magic[2], mftr->magic[3]);

    if (memcmp(mftr->magic, "FILE", 4) != 0) {
        goto found;
    }

    attr_off = le16_to_cpu(mftr->attrs_offset);
    dbg("file $Volume's attributes are at offset %i", attr_off);

    while (1) {
        attr = (struct file_attribute*) &buf[attr_off];
        attr_type = le32_to_cpu(attr->type);
        attr_len = le16_to_cpu(attr->len);
        val_off = le16_to_cpu(attr->value_offset);
        val_len = le32_to_cpu(attr->value_len);
        attr_off += attr_len;

        if (attr_len == 0) {
            break;
        }

        if (attr_off >= mft_record_size) {
            break;
        }

        if (attr_type == MFT_RECORD_ATTR_END) {
            break;
        }

        dbg("found attribute type 0x%x, len %i, at offset %i",
            attr_type, attr_len, attr_off);

        if (attr_type == MFT_RECORD_ATTR_VOLUME_INFO) {
            dbg("found info, len %i", val_len);
            info = (struct volume_info*)(((uint8_t*) attr) + val_off);
            snprintf(id->type_version, sizeof(id->type_version) - 1,
                     "%u.%u", info->major_ver, info->minor_ver);
        }

        if (attr_type == MFT_RECORD_ATTR_VOLUME_NAME) {
            dbg("found label, len %i", val_len);

            if (val_len > VOLUME_ID_LABEL_SIZE) {
                val_len = VOLUME_ID_LABEL_SIZE;
            }

            val = ((uint8_t*) attr) + val_off;
            volume_id_set_label_raw(id, val, val_len);
            volume_id_set_label_unicode16(id, val, LE, val_len);
        }
    }

found:
    volume_id_set_usage(id, VOLUME_ID_FILESYSTEM);
    id->type = "ntfs";

    return 0;
}
