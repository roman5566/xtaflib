#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/reent.h>
#include <diskio/disc_io.h>
#include <sys/iosupport.h>
#include <diskio/ata.h>
#include <debug.h>

#include "xtaf.h"
#include "xtaf_util.h"
#include "xtaf_endian.h"

static const devoptab_t dotab_xtaf = {
	"xtaf",
	sizeof (xtaf_file_private),
	xtaf_open_r,
	xtaf_close_r,
	NULL, // write_r
	xtaf_read_r,
	xtaf_seek_r,
	xtaf_fstat_r,
	xtaf_stat_r,
	NULL, // link_r
	NULL, // unlink_r
	NULL, // chdir_r
	NULL, // rename_r
	NULL, // mkdir_r
	sizeof (xtaf_dir_entry), // dirStateSize
	xtaf_diropen_r, // diropen_r
	NULL, // dirreset_r
	xtaf_dirnext_r, // dirnext_r
	xtaf_dirclose_r, // dirclose_r
	NULL, // statvfs_r
	NULL, // ftruncate_r
	NULL, // fsync_r
	NULL, // deviceData
	NULL, // chmod_r
	NULL  // fchmod_r
};

xtaf_partition_table partition_table[5] = {
	{"hdd0", 0x120eb0000, 0x10000000}, // Xbox 1 Backwards Compatibility
	{"hdd1", 0x130eb0000, 0}, // 360
	{"ext1", 0x10c080000, 0xce30000 }, // System Extended
	{"ext2", 0x118eb0000, 0xce30000 }, // System Extended 2
	{"null", 0, 0}// End
};

//extern xtaf_context *pCtx;

/** return the size of the file alocation table **/
static uint32_t getFatSize(xtaf_partition_private *priv) {

	uint64_t file_system_size = priv->numberOfSectors;
	uint32_t spc = priv->sectorsPerCluster; /* sectors per cluster */

	uint32_t numclusters = (uint32_t) (file_system_size / spc);
	uint8_t fatmult = numclusters >= 0xfff4 ? sizeof (uint32_t) : sizeof (uint16_t);
	uint32_t fatsize = numclusters * fatmult;

	if (fatsize % 4096 != 0)
		fatsize = (((fatsize / 4096) + 1) * 4096); /* round up if not multiple of 4 kB */

	return fatsize / XENON_DISK_SECTOR_SIZE;
}

void print_partition_information(struct xtaf_partition_private *partition) {
	char magic[5];
	memcpy(magic, (const char*) partition->magic, 4);
	magic[4] = 0;

	xprintf("\n\nPartition Information :\n");
	xprintf("fat_file_size       =	%08x\n", partition->fat_file_size * XENON_DISK_SECTOR_SIZE);

	xprintf("magic               =	%s\n", magic);
	xprintf("id                  =	%08x\n", partition->partitionId);
	xprintf("sector_per_cluster  =	%08x\n", partition->sectorsPerCluster);

	xprintf("fat_offset          =	%16lx\n", (partition->fat_offset + partition->partition_start_offset) * XENON_DISK_SECTOR_SIZE);
	xprintf("root_cluster        =	%08x\n", partition->rootDirCluster);
	xprintf("clusters_size       =	%08x\n", partition->bytesPerCluster);
	xprintf("root_offset         =	%16lx\n", (partition->root_offset + partition->partition_start_offset) * XENON_DISK_SECTOR_SIZE);
	
	
	xprintf("fat_offset s         =	%8x\n", (partition->fat_offset + partition->partition_start_offset) );
	xprintf("root_offset s       =%8x\n", (partition->root_offset + partition->partition_start_offset));
}

int xtaf_init_fs(struct xtaf_partition_private *partition) {

	partition->fat_file_size = getFatSize(partition);
	partition->bytesPerSector = XENON_DISK_SECTOR_SIZE;
	partition->bytesPerCluster = partition->sectorsPerCluster* partition->bytesPerSector;
	partition->fat_offset = 0x1000 / XENON_DISK_SECTOR_SIZE;
	partition->root_offset = partition->fat_offset + partition->fat_file_size;
	partition->extent_offset = 0;
	partition->current_sector = 0;
	partition->fatStart =  partition->fat_offset +  partition->partition_start_offset;
	// ???
	partition->rootDirStart = partition->fat_offset + partition->fat_file_size + partition->partition_start_offset;
	partition->dataStart = partition->fat_offset + partition->fat_file_size + partition->partition_start_offset;
        
	print_partition_information(partition);

	return 0;
}

/** return 0 if hdd is retail, 1 if is hdd is devkit **/
int check_devkit_hdd(DISC_INTERFACE * disc) {
    
        bool err;
        uint32_t start_sector = 0;
        uint8_t *sectorBuffer = (uint8_t*) _XTAF_mem_allocate(XENON_DISK_SECTOR_SIZE);
    
        typedef struct xdk_partition_table {
           uint32_t magic;
           uint32_t unknown;
	   uint32_t content_offset;
	   uint32_t content_length;
           uint32_t dashboard_offset;
           uint32_t dashboard_length;
        } xdk_partition_table;
                
        err = _XTAF_disc_readSectors(disc,start_sector,1,sectorBuffer);
        if (err == 0)  {
            xprintf("ERROR: Failed to read @ sec: %u / off: %08X",start_sector,((uint64_t)start_sector * XENON_DISK_SECTOR_SIZE));
            return -1;
        }
        
        struct xdk_partition_table *xdk = (struct xdk_partition_table*) sectorBuffer;
        
        if (xdk->magic != 0x00020000){
			xprintf("No Devkit partition table magic found!\n");
			return 0;
		}
			
        // check for XTAF MAGIC @ content
        start_sector = xdk->content_offset;
        
        err = _XTAF_disc_readSectors(disc,start_sector,1,sectorBuffer);
        if (err == 0)  {
            xprintf("ERROR: Failed to read @ sec: %u / off: %08X",start_sector,((uint64_t)start_sector * XENON_DISK_SECTOR_SIZE));
            return -1;
        }
            
        if (memcmp(sectorBuffer, "XTAF", 4)){
            xprintf("No XTAF Magic at content partition offset!\n");
            return -1;
        }

        //check for XTAF MAGIC @ dashboard
        start_sector = xdk->dashboard_offset;
        
        err = _XTAF_disc_readSectors(disc,start_sector,1,sectorBuffer);
        if (err == 0)  {
            xprintf("ERROR: Failed to read @ sec: %u / off: %08X",start_sector,((uint64_t)start_sector * XENON_DISK_SECTOR_SIZE));
            return -1;
        }
            
        if (memcmp(sectorBuffer, "XTAF", 4)){
            xprintf("No XTAF Magic at dashboard partition offset!\n");
            return -1;
        }
        
        // Assign new partition values
        partition_table[0].offset = (uint64_t) xdk->content_offset * XENON_DISK_SECTOR_SIZE;
        partition_table[0].length = (uint64_t) xdk->content_length * XENON_DISK_SECTOR_SIZE;
        partition_table[1].offset = (uint64_t) xdk->dashboard_offset * XENON_DISK_SECTOR_SIZE;
        partition_table[1].length = (uint64_t) xdk->dashboard_length * XENON_DISK_SECTOR_SIZE;

        
	return 1;
}

xtaf_partition_private * xtaf_mount(void * disc, uint32_t start_sector, uint32_t num_sectors, uint32_t cacheSize, uint32_t sectorsPerPage, uint8_t *sectorBuffer ) {
	bool err;
	static int partition_nbr = 0;
	devoptab_t* devops;
	char name[]="sda0";	
	name[3] = partition_nbr+'0';

	xtaf_partition_private * partition = (xtaf_partition_private*)_XTAF_mem_allocate(sizeof(xtaf_partition_private));

	partition->disc = disc;

	partition->partitionNumber = partition_nbr;

	partition->partition_start_offset = start_sector;

	partition->numberOfSectors = num_sectors;

	// Read the header
	//err = ioread_sector(partition, sectorBuffer, 0, 1);
	
	err = _XTAF_disc_readSectors(disc,start_sector,1,sectorBuffer);

	if (err==false) {
		_XTAF_mem_free(partition);
		return NULL;
	}

	if (memcmp(sectorBuffer, "XTAF", 4) == 0) {
		xprintf("found a parition at %08x\n",start_sector);
		
		struct _xtaf_partition_hdr_s * p = (struct _xtaf_partition_hdr_s*) sectorBuffer;

		
		memcpy(partition->magic, p->magic, 4);
		partition->partitionName = partition_table[partition_nbr].name;
		
		// Bswap partition header (for little endian cpu)
		partition->partitionId = host2be32(p->id);
		partition->sectorsPerCluster = host2be32(p->sector_per_cluster);
		partition->rootDirCluster =host2be32(p->root_cluster);
		
		partition->found = 1;
		
		//return xtaf_init_fs(priv);
		xtaf_init_fs(partition);
	} else {
		_XTAF_mem_free(partition);
		return NULL;
	}

	partition->cache = _XTAF_cache_constructor(cacheSize, sectorsPerPage, partition->disc, start_sector + num_sectors, 0x200);

	partition_nbr++;
	

	devops = _XTAF_mem_allocate (sizeof(devoptab_t) + strlen(name) + 1);
	
	// Use the space allocated at the end of the devoptab struct for storing the name
	char * nameCopy = (char*)(devops+1);
	
	// Add an entry for this device to the devoptab table
	memcpy (devops, &dotab_xtaf, sizeof(dotab_xtaf));
	strcpy (nameCopy, name);
	devops->name = nameCopy;
	devops->deviceData = partition;

	err =  AddDevice (devops);
	
	xprintf("err = %d\r\n",err);

	return partition;
}

/** xtaf_init init sata, and look for xtaf partition **/
int xtaf_init(struct xtaf_context *ctx, DISC_INTERFACE * disc) {
	/*
		if (pCtx == NULL) {
			pCtx = ctx;
		}
	 */
	ctx->dev = disc;
	if (disc == NULL) {
		xprintf("disc is nulll\r\n\r\n");
	}
	

	// start disc
	disc->startup();

	int partition_nbr = 0;

	if (check_devkit_hdd(disc) == 1)
		xprintf("DEVKIT HDD detected!\r\n");
        
        uint8_t *sectorBuffer = (uint8_t*) _XTAF_mem_allocate(XENON_DISK_SECTOR_SIZE);

	int found = 0;

	// use only 1 parition for now ...
	while (1) {
		uint32_t start_sector, num_sectors;

		start_sector = partition_table[partition_nbr].offset / XENON_DISK_SECTOR_SIZE;

		if (partition_table[partition_nbr].length == 0) {
#ifdef XENON
			//num_sectors = (uint32_t) dev->size - start_sector;
			//num_sectors = (uint32_t)  (312581808) - start_sector;
			extern struct xenon_ata_device ata;
			num_sectors = (uint32_t)  (ata.size) - start_sector;
#else
			num_sectors = (20003880960 / XENON_DISK_SECTOR_SIZE) - start_sector; // hdd_dump file size
#endif
		} else {
			num_sectors = (partition_table[partition_nbr].length) / XENON_DISK_SECTOR_SIZE;
		}

		xtaf_partition_private * partition =xtaf_mount(disc,start_sector, num_sectors, DEFAULT_CACHE_PAGES, DEFAULT_SECTORS_PAGE,sectorBuffer);

		if(partition==NULL){
			//error
		}
		else{
			// OK
			found++;
		}

		ctx->priv[partition_nbr] = partition;

		partition_nbr++;
		if (partition_table[partition_nbr].offset == 0)
			break;
	}

	_XTAF_mem_free(sectorBuffer);

	return found;
}
