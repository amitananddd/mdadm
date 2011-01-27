/*
 * mdadm - manage Linux "md" devices aka RAID arrays.
 *
 * Copyright (C) 2001-2009 Neil Brown <neilb@suse.de>
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    Author: Neil Brown
 *    Email: <neilb@suse.de>
 */
#include	"mdadm.h"
#include	"dlink.h"
#include	<sys/mman.h>

#if ! defined(__BIG_ENDIAN) && ! defined(__LITTLE_ENDIAN)
#error no endian defined
#endif
#include	"md_u.h"
#include	"md_p.h"

#ifndef offsetof
#define offsetof(t,f) ((size_t)&(((t*)0)->f))
#endif

int Grow_Add_device(char *devname, int fd, char *newdev)
{
	/* Add a device to an active array.
	 * Currently, just extend a linear array.
	 * This requires writing a new superblock on the
	 * new device, calling the kernel to add the device,
	 * and if that succeeds, update the superblock on
	 * all other devices.
	 * This means that we need to *find* all other devices.
	 */
	struct mdinfo info;

	struct stat stb;
	int nfd, fd2;
	int d, nd;
	struct supertype *st = NULL;
	char *subarray = NULL;

	if (ioctl(fd, GET_ARRAY_INFO, &info.array) < 0) {
		fprintf(stderr, Name ": cannot get array info for %s\n", devname);
		return 1;
	}

	if (info.array.level != -1) {
		fprintf(stderr, Name ": can only add devices to linear arrays\n");
		return 1;
	}

	st = super_by_fd(fd, &subarray);
	if (!st) {
		fprintf(stderr, Name ": cannot handle arrays with superblock version %d\n", info.array.major_version);
		return 1;
	}

	if (subarray) {
		fprintf(stderr, Name ": Cannot grow linear sub-arrays yet\n");
		free(subarray);
		free(st);
	}

	nfd = open(newdev, O_RDWR|O_EXCL|O_DIRECT);
	if (nfd < 0) {
		fprintf(stderr, Name ": cannot open %s\n", newdev);
		free(st);
		return 1;
	}
	fstat(nfd, &stb);
	if ((stb.st_mode & S_IFMT) != S_IFBLK) {
		fprintf(stderr, Name ": %s is not a block device!\n", newdev);
		close(nfd);
		free(st);
		return 1;
	}
	/* now check out all the devices and make sure we can read the superblock */
	for (d=0 ; d < info.array.raid_disks ; d++) {
		mdu_disk_info_t disk;
		char *dv;

		st->ss->free_super(st);

		disk.number = d;
		if (ioctl(fd, GET_DISK_INFO, &disk) < 0) {
			fprintf(stderr, Name ": cannot get device detail for device %d\n",
				d);
			close(nfd);
			free(st);
			return 1;
		}
		dv = map_dev(disk.major, disk.minor, 1);
		if (!dv) {
			fprintf(stderr, Name ": cannot find device file for device %d\n",
				d);
			close(nfd);
			free(st);
			return 1;
		}
		fd2 = dev_open(dv, O_RDWR);
		if (!fd2) {
			fprintf(stderr, Name ": cannot open device file %s\n", dv);
			close(nfd);
			free(st);
			return 1;
		}

		if (st->ss->load_super(st, fd2, NULL)) {
			fprintf(stderr, Name ": cannot find super block on %s\n", dv);
			close(nfd);
			close(fd2);
			free(st);
			return 1;
		}
		close(fd2);
	}
	/* Ok, looks good. Lets update the superblock and write it out to
	 * newdev.
	 */

	info.disk.number = d;
	info.disk.major = major(stb.st_rdev);
	info.disk.minor = minor(stb.st_rdev);
	info.disk.raid_disk = d;
	info.disk.state = (1 << MD_DISK_SYNC) | (1 << MD_DISK_ACTIVE);
	st->ss->update_super(st, &info, "linear-grow-new", newdev,
			     0, 0, NULL);

	if (st->ss->store_super(st, nfd)) {
		fprintf(stderr, Name ": Cannot store new superblock on %s\n",
			newdev);
		close(nfd);
		return 1;
	}
	close(nfd);

	if (ioctl(fd, ADD_NEW_DISK, &info.disk) != 0) {
		fprintf(stderr, Name ": Cannot add new disk to this array\n");
		return 1;
	}
	/* Well, that seems to have worked.
	 * Now go through and update all superblocks
	 */

	if (ioctl(fd, GET_ARRAY_INFO, &info.array) < 0) {
		fprintf(stderr, Name ": cannot get array info for %s\n", devname);
		return 1;
	}

	nd = d;
	for (d=0 ; d < info.array.raid_disks ; d++) {
		mdu_disk_info_t disk;
		char *dv;

		disk.number = d;
		if (ioctl(fd, GET_DISK_INFO, &disk) < 0) {
			fprintf(stderr, Name ": cannot get device detail for device %d\n",
				d);
			return 1;
		}
		dv = map_dev(disk.major, disk.minor, 1);
		if (!dv) {
			fprintf(stderr, Name ": cannot find device file for device %d\n",
				d);
			return 1;
		}
		fd2 = dev_open(dv, O_RDWR);
		if (fd2 < 0) {
			fprintf(stderr, Name ": cannot open device file %s\n", dv);
			return 1;
		}
		if (st->ss->load_super(st, fd2, NULL)) {
			fprintf(stderr, Name ": cannot find super block on %s\n", dv);
			close(fd);
			return 1;
		}
		info.array.raid_disks = nd+1;
		info.array.nr_disks = nd+1;
		info.array.active_disks = nd+1;
		info.array.working_disks = nd+1;

		st->ss->update_super(st, &info, "linear-grow-update", dv,
				     0, 0, NULL);

		if (st->ss->store_super(st, fd2)) {
			fprintf(stderr, Name ": Cannot store new superblock on %s\n", dv);
			close(fd2);
			return 1;
		}
		close(fd2);
	}

	return 0;
}

int Grow_addbitmap(char *devname, int fd, char *file, int chunk, int delay, int write_behind, int force)
{
	/*
	 * First check that array doesn't have a bitmap
	 * Then create the bitmap
	 * Then add it
	 *
	 * For internal bitmaps, we need to check the version,
	 * find all the active devices, and write the bitmap block
	 * to all devices
	 */
	mdu_bitmap_file_t bmf;
	mdu_array_info_t array;
	struct supertype *st;
	char *subarray = NULL;
	int major = BITMAP_MAJOR_HI;
	int vers = md_get_version(fd);
	unsigned long long bitmapsize, array_size;

	if (vers < 9003) {
		major = BITMAP_MAJOR_HOSTENDIAN;
		fprintf(stderr, Name ": Warning - bitmaps created on this kernel"
			" are not portable\n"
			"  between different architectures.  Consider upgrading"
			" the Linux kernel.\n");
	}

	if (ioctl(fd, GET_BITMAP_FILE, &bmf) != 0) {
		if (errno == ENOMEM)
			fprintf(stderr, Name ": Memory allocation failure.\n");
		else
			fprintf(stderr, Name ": bitmaps not supported by this kernel.\n");
		return 1;
	}
	if (bmf.pathname[0]) {
		if (strcmp(file,"none")==0) {
			if (ioctl(fd, SET_BITMAP_FILE, -1)!= 0) {
				fprintf(stderr, Name ": failed to remove bitmap %s\n",
					bmf.pathname);
				return 1;
			}
			return 0;
		}
		fprintf(stderr, Name ": %s already has a bitmap (%s)\n",
			devname, bmf.pathname);
		return 1;
	}
	if (ioctl(fd, GET_ARRAY_INFO, &array) != 0) {
		fprintf(stderr, Name ": cannot get array status for %s\n", devname);
		return 1;
	}
	if (array.state & (1<<MD_SB_BITMAP_PRESENT)) {
		if (strcmp(file, "none")==0) {
			array.state &= ~(1<<MD_SB_BITMAP_PRESENT);
			if (ioctl(fd, SET_ARRAY_INFO, &array)!= 0) {
				fprintf(stderr, Name ": failed to remove internal bitmap.\n");
				return 1;
			}
			return 0;
		}
		fprintf(stderr, Name ": Internal bitmap already present on %s\n",
			devname);
		return 1;
	}

	if (strcmp(file, "none") == 0) {
		fprintf(stderr, Name ": no bitmap found on %s\n", devname);
		return 1;
	}
	if (array.level <= 0) {
		fprintf(stderr, Name ": Bitmaps not meaningful with level %s\n",
			map_num(pers, array.level)?:"of this array");
		return 1;
	}
	bitmapsize = array.size;
	bitmapsize <<= 1;
	if (get_dev_size(fd, NULL, &array_size) &&
	    array_size > (0x7fffffffULL<<9)) {
		/* Array is big enough that we cannot trust array.size
		 * try other approaches
		 */
		bitmapsize = get_component_size(fd);
	}
	if (bitmapsize == 0) {
		fprintf(stderr, Name ": Cannot reliably determine size of array to create bitmap - sorry.\n");
		return 1;
	}

	if (array.level == 10) {
		int ncopies = (array.layout&255)*((array.layout>>8)&255);
		bitmapsize = bitmapsize * array.raid_disks / ncopies;
	}

	st = super_by_fd(fd, &subarray);
	if (!st) {
		fprintf(stderr, Name ": Cannot understand version %d.%d\n",
			array.major_version, array.minor_version);
		return 1;
	}
	if (subarray) {
		fprintf(stderr, Name ": Cannot add bitmaps to sub-arrays yet\n");
		free(subarray);
		free(st);
		return 1;
	}
	if (strcmp(file, "internal") == 0) {
		int d;
		if (st->ss->add_internal_bitmap == NULL) {
			fprintf(stderr, Name ": Internal bitmaps not supported "
				"with %s metadata\n", st->ss->name);
			return 1;
		}
		for (d=0; d< st->max_devs; d++) {
			mdu_disk_info_t disk;
			char *dv;
			disk.number = d;
			if (ioctl(fd, GET_DISK_INFO, &disk) < 0)
				continue;
			if (disk.major == 0 &&
			    disk.minor == 0)
				continue;
			if ((disk.state & (1<<MD_DISK_SYNC))==0)
				continue;
			dv = map_dev(disk.major, disk.minor, 1);
			if (dv) {
				int fd2 = dev_open(dv, O_RDWR);
				if (fd2 < 0)
					continue;
				if (st->ss->load_super(st, fd2, NULL)==0) {
					if (st->ss->add_internal_bitmap(
						    st,
						    &chunk, delay, write_behind,
						    bitmapsize, 0, major)
						)
						st->ss->write_bitmap(st, fd2);
					else {
						fprintf(stderr, Name ": failed to create internal bitmap - chunksize problem.\n");
						close(fd2);
						return 1;
					}
				}
				close(fd2);
			}
		}
		array.state |= (1<<MD_SB_BITMAP_PRESENT);
		if (ioctl(fd, SET_ARRAY_INFO, &array)!= 0) {
			if (errno == EBUSY)
				fprintf(stderr, Name
					": Cannot add bitmap while array is"
					" resyncing or reshaping etc.\n");
			fprintf(stderr, Name ": failed to set internal bitmap.\n");
			return 1;
		}
	} else {
		int uuid[4];
		int bitmap_fd;
		int d;
		int max_devs = st->max_devs;

		/* try to load a superblock */
		for (d=0; d<max_devs; d++) {
			mdu_disk_info_t disk;
			char *dv;
			int fd2;
			disk.number = d;
			if (ioctl(fd, GET_DISK_INFO, &disk) < 0)
				continue;
			if ((disk.major==0 && disk.minor==0) ||
			    (disk.state & (1<<MD_DISK_REMOVED)))
				continue;
			dv = map_dev(disk.major, disk.minor, 1);
			if (!dv) continue;
			fd2 = dev_open(dv, O_RDONLY);
			if (fd2 >= 0 &&
			    st->ss->load_super(st, fd2, NULL) == 0) {
				close(fd2);
				st->ss->uuid_from_super(st, uuid);
				break;
			}
			close(fd2);
		}
		if (d == max_devs) {
			fprintf(stderr, Name ": cannot find UUID for array!\n");
			return 1;
		}
		if (CreateBitmap(file, force, (char*)uuid, chunk,
				 delay, write_behind, bitmapsize, major)) {
			return 1;
		}
		bitmap_fd = open(file, O_RDWR);
		if (bitmap_fd < 0) {
			fprintf(stderr, Name ": weird: %s cannot be opened\n",
				file);
			return 1;
		}
		if (ioctl(fd, SET_BITMAP_FILE, bitmap_fd) < 0) {
			int err = errno;
			if (errno == EBUSY)
				fprintf(stderr, Name
					": Cannot add bitmap while array is"
					" resyncing or reshaping etc.\n");
			fprintf(stderr, Name ": Cannot set bitmap file for %s: %s\n",
				devname, strerror(err));
			return 1;
		}
	}

	return 0;
}


/*
 * When reshaping an array we might need to backup some data.
 * This is written to all spares with a 'super_block' describing it.
 * The superblock goes 4K from the end of the used space on the
 * device.
 * It if written after the backup is complete.
 * It has the following structure.
 */

static struct mdp_backup_super {
	char	magic[16];  /* md_backup_data-1 or -2 */
	__u8	set_uuid[16];
	__u64	mtime;
	/* start/sizes in 512byte sectors */
	__u64	devstart;	/* address on backup device/file of data */
	__u64	arraystart;
	__u64	length;
	__u32	sb_csum;	/* csum of preceeding bytes. */
	__u32   pad1;
	__u64	devstart2;	/* offset in to data of second section */
	__u64	arraystart2;
	__u64	length2;
	__u32	sb_csum2;	/* csum of preceeding bytes. */
	__u8 pad[512-68-32];
} __attribute__((aligned(512))) bsb, bsb2;

static __u32 bsb_csum(char *buf, int len)
{
	int i;
	int csum = 0;
	for (i=0; i<len; i++)
		csum = (csum<<3) + buf[0];
	return __cpu_to_le32(csum);
}

static int check_idle(struct supertype *st)
{
	/* Check that all member arrays for this container, or the
	 * container of this array, are idle
	 */
	int container_dev = (st->container_dev != NoMdDev
			     ? st->container_dev : st->devnum);
	char container[40];
	struct mdstat_ent *ent, *e;
	int is_idle = 1;
	
	fmt_devname(container, container_dev);
	ent = mdstat_read(0, 0);
	for (e = ent ; e; e = e->next) {
		if (!is_container_member(e, container))
			continue;
		if (e->percent >= 0) {
			is_idle = 0;
			break;
		}
	}
	free_mdstat(ent);
	return is_idle;
}

static int freeze_container(struct supertype *st)
{
	int container_dev = (st->container_dev != NoMdDev
			     ? st->container_dev : st->devnum);
	char container[40];

	if (!check_idle(st))
		return -1;
	
	fmt_devname(container, container_dev);

	if (block_monitor(container, 1)) {
		fprintf(stderr, Name ": failed to freeze container\n");
		return -2;
	}

	return 1;
}

static void unfreeze_container(struct supertype *st)
{
	int container_dev = (st->container_dev != NoMdDev
			     ? st->container_dev : st->devnum);
	char container[40];
	
	fmt_devname(container, container_dev);

	unblock_monitor(container, 1);
}

static int freeze(struct supertype *st)
{
	/* Try to freeze resync/rebuild on this array/container.
	 * Return -1 if the array is busy,
	 * return -2 container cannot be frozen,
	 * return 0 if this kernel doesn't support 'frozen'
	 * return 1 if it worked.
	 */
	if (st->ss->external)
		return freeze_container(st);
	else {
		struct mdinfo *sra = sysfs_read(-1, st->devnum, GET_VERSION);
		int err;

		if (!sra)
			return -1;
		err = sysfs_freeze_array(sra);
		sysfs_free(sra);
		return err;
	}
}

static void unfreeze(struct supertype *st)
{
	if (st->ss->external)
		return unfreeze_container(st);
	else {
		struct mdinfo *sra = sysfs_read(-1, st->devnum, GET_VERSION);

		if (sra)
			sysfs_set_str(sra, NULL, "sync_action", "idle");
		else
			fprintf(stderr, Name ": failed to unfreeze array\n");
		sysfs_free(sra);
	}
}

static void wait_reshape(struct mdinfo *sra)
{
	int fd = sysfs_get_fd(sra, NULL, "sync_action");
	char action[20];

	if (fd < 0)
		return;

	while  (sysfs_fd_get_str(fd, action, 20) > 0 &&
		strncmp(action, "reshape", 7) == 0) {
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		select(fd+1, NULL, NULL, &rfds, NULL);
	}
	close(fd);
}

static int reshape_super(struct supertype *st, long long size, int level,
			 int layout, int chunksize, int raid_disks,
			 char *backup_file, char *dev, int verbose)
{
	/* nothing extra to check in the native case */
	if (!st->ss->external)
		return 0;
	if (!st->ss->reshape_super ||
	    !st->ss->manage_reshape) {
		fprintf(stderr, Name ": %s metadata does not support reshape\n",
			st->ss->name);
		return 1;
	}

	return st->ss->reshape_super(st, size, level, layout, chunksize,
				     raid_disks, backup_file, dev, verbose);
}

static void sync_metadata(struct supertype *st)
{
	if (st->ss->external) {
		if (st->update_tail) {
			flush_metadata_updates(st);
			st->update_tail = &st->updates;
		} else
			st->ss->sync_metadata(st);
	}
}

static int subarray_set_num(char *container, struct mdinfo *sra, char *name, int n)
{
	/* when dealing with external metadata subarrays we need to be
	 * prepared to handle EAGAIN.  The kernel may need to wait for
	 * mdmon to mark the array active so the kernel can handle
	 * allocations/writeback when preparing the reshape action
	 * (md_allow_write()).  We temporarily disable safe_mode_delay
	 * to close a race with the array_state going clean before the
	 * next write to raid_disks / stripe_cache_size
	 */
	char safe[50];
	int rc;

	/* only 'raid_disks' and 'stripe_cache_size' trigger md_allow_write */
	if (!container ||
	    (strcmp(name, "raid_disks") != 0 &&
	     strcmp(name, "stripe_cache_size") != 0))
		return sysfs_set_num(sra, NULL, name, n);

	rc = sysfs_get_str(sra, NULL, "safe_mode_delay", safe, sizeof(safe));
	if (rc <= 0)
		return -1;
	sysfs_set_num(sra, NULL, "safe_mode_delay", 0);
	rc = sysfs_set_num(sra, NULL, name, n);
	if (rc < 0 && errno == EAGAIN) {
		ping_monitor(container);
		/* if we get EAGAIN here then the monitor is not active
		 * so stop trying
		 */
		rc = sysfs_set_num(sra, NULL, name, n);
	}
	sysfs_set_str(sra, NULL, "safe_mode_delay", safe);
	return rc;
}

int start_reshape(struct mdinfo *sra)
{
	int err;
	sysfs_set_num(sra, NULL, "suspend_lo", 0x7FFFFFFFFFFFFFFFULL);
	err = sysfs_set_num(sra, NULL, "suspend_hi", 0);
	err = err ?: sysfs_set_num(sra, NULL, "suspend_lo", 0);
	/* Setting sync_min can fail if the recovery is already 'running',
	 * which can happen when restarting an array which is reshaping.
	 * So don't worry about errors here */
	sysfs_set_num(sra, NULL, "sync_min", 0);
	err = err ?: sysfs_set_num(sra, NULL, "sync_max", 0);
	err = err ?: sysfs_set_str(sra, NULL, "sync_action", "reshape");

	return err;
}

void abort_reshape(struct mdinfo *sra)
{
	sysfs_set_str(sra, NULL, "sync_action", "idle");
	sysfs_set_num(sra, NULL, "suspend_lo", 0x7FFFFFFFFFFFFFFFULL);
	sysfs_set_num(sra, NULL, "suspend_hi", 0);
	sysfs_set_num(sra, NULL, "suspend_lo", 0);
	sysfs_set_num(sra, NULL, "sync_min", 0);
	sysfs_set_str(sra, NULL, "sync_max", "max");
}

int remove_disks_for_takeover(struct supertype *st,
			      struct mdinfo *sra,
			      int layout)
{
	int nr_of_copies;
	struct mdinfo *remaining;
	int slot;

	if (sra->array.level == 10)
		nr_of_copies = layout & 0xff;
	else if (sra->array.level == 1)
		nr_of_copies = sra->array.raid_disks;
	else
		return 1;

	remaining = sra->devs;
	sra->devs = NULL;
	/* for each 'copy', select one device and remove from the list. */
	for (slot = 0; slot < sra->array.raid_disks; slot += nr_of_copies) {
		struct mdinfo **diskp;
		int found = 0;

		/* Find a working device to keep */
		for (diskp =  &remaining; *diskp ; diskp = &(*diskp)->next) {
			struct mdinfo *disk = *diskp;

			if (disk->disk.raid_disk < slot)
				continue;
			if (disk->disk.raid_disk >= slot + nr_of_copies)
				continue;
			if (disk->disk.state & (1<<MD_DISK_REMOVED))
				continue;
			if (disk->disk.state & (1<<MD_DISK_FAULTY))
				continue;
			if (!(disk->disk.state & (1<<MD_DISK_SYNC)))
				continue;

			/* We have found a good disk to use! */
			*diskp = disk->next;
			disk->next = sra->devs;
			sra->devs = disk;
			found = 1;
			break;
		}
		if (!found)
			break;
	}

	if (slot < sra->array.raid_disks) {
		/* didn't find all slots */
		struct mdinfo **e;
		e = &remaining;
		while (*e)
			e = &(*e)->next;
		*e = sra->devs;
		sra->devs = remaining;
		return 1;
	}

	/* Remove all 'remaining' devices from the array */
	while (remaining) {
		struct mdinfo *sd = remaining;
		remaining = sd->next;

		sysfs_set_str(sra, sd, "state", "faulty");
		sysfs_set_str(sra, sd, "slot", "none");
		sysfs_set_str(sra, sd, "state", "remove");
		sd->disk.state |= (1<<MD_DISK_REMOVED);
		sd->disk.state &= ~(1<<MD_DISK_SYNC);
		sd->next = sra->devs;
		sra->devs = sd;
	}
	return 0;
}

void reshape_free_fdlist(int *fdlist,
			 unsigned long long *offsets,
			 int size)
{
	int i;

	for (i = 0; i < size; i++)
		if (fdlist[i] >= 0)
			close(fdlist[i]);

	free(fdlist);
	free(offsets);
}

int reshape_prepare_fdlist(char *devname,
			   struct mdinfo *sra,
			   int raid_disks,
			   int nrdisks,
			   unsigned long blocks,
			   char *backup_file,
			   int *fdlist,
			   unsigned long long *offsets)
{
	int d = 0;
	struct mdinfo *sd;

	for (d = 0; d <= nrdisks; d++)
		fdlist[d] = -1;
	d = raid_disks;
	for (sd = sra->devs; sd; sd = sd->next) {
		if (sd->disk.state & (1<<MD_DISK_FAULTY))
			continue;
		if (sd->disk.state & (1<<MD_DISK_SYNC)) {
			char *dn = map_dev(sd->disk.major,
					   sd->disk.minor, 1);
			fdlist[sd->disk.raid_disk]
				= dev_open(dn, O_RDONLY);
			offsets[sd->disk.raid_disk] = sd->data_offset*512;
			if (fdlist[sd->disk.raid_disk] < 0) {
				fprintf(stderr,
					Name ": %s: cannot open component %s\n",
					devname, dn ? dn : "-unknown-");
				d = -1;
				goto release;
			}
		} else if (backup_file == NULL) {
			/* spare */
			char *dn = map_dev(sd->disk.major,
					   sd->disk.minor, 1);
				fdlist[d] = dev_open(dn, O_RDWR);
				offsets[d] = (sd->data_offset + sra->component_size - blocks - 8)*512;
				if (fdlist[d] < 0) {
					fprintf(stderr, Name ": %s: cannot open component %s\n",
						devname, dn ? dn : "-unknown-");
					d = -1;
					goto release;
				}
				d++;
			}
		}
release:
	return d;
}

int reshape_open_backup_file(char *backup_file,
			     int fd,
			     char *devname,
			     long blocks,
			     int *fdlist,
			     unsigned long long *offsets,
			     int restart)
{
	/* Return 1 on success, 0 on any form of failure */
	/* need to check backup file is large enough */
	char buf[512];
	struct stat stb;
	unsigned int dev;
	int i;

	*fdlist = open(backup_file, O_RDWR|O_CREAT|(restart ? O_TRUNC : O_EXCL),
		       S_IRUSR | S_IWUSR);
	*offsets = 8 * 512;
	if (*fdlist < 0) {
		fprintf(stderr, Name ": %s: cannot create backup file %s: %s\n",
			devname, backup_file, strerror(errno));
		return 0;
	}
	/* Guard against backup file being on array device.
	 * If array is partitioned or if LVM etc is in the
	 * way this will not notice, but it is better than
	 * nothing.
	 */
	fstat(*fdlist, &stb);
	dev = stb.st_dev;
	fstat(fd, &stb);
	if (stb.st_rdev == dev) {
		fprintf(stderr, Name ": backup file must NOT be"
			" on the array being reshaped.\n");
		close(*fdlist);
		return 0;
	}

	memset(buf, 0, 512);
	for (i=0; i < blocks + 1 ; i++) {
		if (write(*fdlist, buf, 512) != 512) {
			fprintf(stderr, Name ": %s: cannot create"
				" backup file %s: %s\n",
				devname, backup_file, strerror(errno));
			return 0;
		}
	}
	if (fsync(*fdlist) != 0) {
		fprintf(stderr, Name ": %s: cannot create backup file %s: %s\n",
			devname, backup_file, strerror(errno));
		return 0;
	}

	return 1;
}

unsigned long compute_backup_blocks(int nchunk, int ochunk,
				    unsigned int ndata, unsigned int odata)
{
	unsigned long a, b, blocks;
	/* So how much do we need to backup.
	 * We need an amount of data which is both a whole number of
	 * old stripes and a whole number of new stripes.
	 * So LCM for (chunksize*datadisks).
	 */
	a = (ochunk/512) * odata;
	b = (nchunk/512) * ndata;
	/* Find GCD */
	while (a != b) {
		if (a < b)
			b -= a;
		if (b < a)
			a -= b;
	}
	/* LCM == product / GCD */
	blocks = (ochunk/512) * (nchunk/512) * odata * ndata / a;

	return blocks;
}

char *analyse_change(struct mdinfo *info, struct reshape *re)
{
	/* Based on the current array state in info->array and
	 * the changes in info->new_* etc, determine:
	 *  - whether the change is possible
	 *  - Intermediate level/raid_disks/layout
	 *  - whether a restriping reshape is needed
	 *  - number of sectors in minimum change unit.  This
	 *    will cover a whole number of stripes in 'before' and
	 *    'after'.
	 *
	 * Return message if the change should be rejected
	 *        NULL if the change can be achieved
	 *
	 * This can be called as part of starting a reshape, or
	 * when assembling an array that is undergoing reshape.
	 */
	int new_disks;

	/* If a new level not explicitly given, we assume no-change */
	if (info->new_level == UnSet)
		info->new_level = info->array.level;

	if (info->new_chunk)
		switch (info->new_level) {
		case 0:
		case 4:
		case 5:
		case 6:
		case 10:
			/* chunk size is meaningful, must divide component_size
			 * evenly
			 */
			if (info->component_size % (info->new_chunk/512))
				return "New chunk size does not"
					" divide component size";
			break;
		default:
			return "chunk size not meaningful for this level";
		}
	else
		info->new_chunk = info->array.chunk_size;

	switch (info->array.level) {
	case 1:
		/* RAID1 can convert to RAID1 with different disks, or
		 * raid5 with 2 disks, or
		 * raid0 with 1 disk
		 */
		if (info->new_level == 0) {
			re->level = 0;
			re->before.data_disks = 1;
			re->after.data_disks = 1;
			re->before.layout = 0;
			re->backup_blocks = 0;
			re->parity = 0;
			return NULL;
		}
		if (info->new_level == 1) {
			if (info->delta_disks == UnSet)
				/* Don't know what to do */
				return "no change requested for Growing RAID1";
			re->level = 1;
			re->backup_blocks = 0;
			re->parity = 0;
			return NULL;
		}
		if (info->array.raid_disks == 2 &&
		    info->new_level == 5) {
			re->level = 5;
			re->before.data_disks = 1;
			re->after.data_disks = 1;
			re->before.layout = ALGORITHM_LEFT_SYMMETRIC;
			info->array.chunk_size = 65536;
			break;
		}
		/* Could do some multi-stage conversions, but leave that to
		 * later.
		 */
		return "Impossibly level change request for RAID1";

	case 10:
		/* RAID10 can only be converted from near mode to
		 * RAID0 by removing some devices
		 */
		if ((info->array.layout & ~0xff) != 0x100)
			return "Cannot Grow RAID10 with far/offset layout";
		/* number of devices must be multiple of number of copies */
		if (info->array.raid_disks % (info->array.layout & 0xff))
			return "RAID10 layout too complex for Grow operation";

		if (info->new_level != 0)
			return "RAID10 can only be changed to RAID0";
		new_disks = (info->array.raid_disks
			     / (info->array.layout & 0xff));
		if (info->delta_disks == UnSet) {
			info->delta_disks = (new_disks
					     - info->array.raid_disks);
		}
		if (info->delta_disks != new_disks - info->array.raid_disks)
			return "New number of raid-devices impossible for RAID10";
		if (info->new_chunk &&
		    info->new_chunk != info->array.chunk_size)
			return "Cannot change chunk-size with RAID10 Grow";

		/* looks good */
		re->level = 0;
		re->parity = 0;
		re->before.data_disks = new_disks;
		re->after.data_disks = re->before.data_disks;
		re->before.layout = 0;
		re->backup_blocks = 0;
		return NULL;

	case 0:
		/* RAID0 can be converted to RAID10, or to RAID456 */
		if (info->new_level == 10) {
			if (info->new_layout == UnSet && info->delta_disks == UnSet) {
				/* Assume near=2 layout */
				info->new_layout = 0x102;
				info->delta_disks = info->array.raid_disks;
			}
			if (info->new_layout == UnSet) {
				int copies = 1 + (info->delta_disks
						  / info->array.raid_disks);
				if (info->array.raid_disks * (copies-1)
				    != info->delta_disks)
					return "Impossible number of devices"
						" for RAID0->RAID10";
				info->new_layout = 0x100 + copies;
			}
			if (info->delta_disks == UnSet) {
				int copies = info->new_layout & 0xff;
				if (info->new_layout != 0x100 + copies)
					return "New layout impossible"
						" for RAID0->RAID10";;
				info->delta_disks = (copies - 1) *
					info->array.raid_disks;
			}
			if (info->new_chunk &&
			    info->new_chunk != info->array.chunk_size)
				return "Cannot change chunk-size with RAID0->RAID10";
			/* looks good */
			re->level = 10;
			re->parity = 0;
			re->before.data_disks = (info->array.raid_disks +
						 info->delta_disks);
			re->after.data_disks = re->before.data_disks;
			re->before.layout = info->new_layout;
			re->backup_blocks = 0;
			return NULL;
		}

		/* RAID0 can also covert to RAID0/4/5/6 by first converting to
		 * a raid4 style layout of the final level.
		 */
		switch (info->new_level) {
		case 0:
		case 4:
			re->level = 4;
			re->before.layout = 0;
			break;
		case 5:
			re->level = 5;
			re->before.layout = ALGORITHM_PARITY_N;
			break;
		case 6:
			re->level = 6;
			re->before.layout = ALGORITHM_PARITY_N;
			break;
		default:
			return "Impossible level change requested";
		}
		re->before.data_disks = info->array.raid_disks;
		/* determining 'after' layout happens outside this 'switch' */
		break;

	case 4:
		info->array.layout = ALGORITHM_PARITY_N;
	case 5:
		switch (info->new_level) {
		case 4:
			re->level = info->array.level;
			re->before.data_disks = info->array.raid_disks - 1;
			re->before.layout = info->array.layout;
			break;
		case 5:
			re->level = 5;
			re->before.data_disks = info->array.raid_disks - 1;
			re->before.layout = info->array.layout;
			break;
		case 6:
			re->level = 6;
			re->before.data_disks = info->array.raid_disks - 1;
			switch (info->array.layout) {
			case ALGORITHM_LEFT_ASYMMETRIC:
				re->before.layout = ALGORITHM_LEFT_ASYMMETRIC_6;
				break;
			case ALGORITHM_RIGHT_ASYMMETRIC:
				re->before.layout = ALGORITHM_RIGHT_ASYMMETRIC_6;
				break;
			case ALGORITHM_LEFT_SYMMETRIC:
				re->before.layout = ALGORITHM_LEFT_SYMMETRIC_6;
				break;
			case ALGORITHM_RIGHT_SYMMETRIC:
				re->before.layout = ALGORITHM_RIGHT_SYMMETRIC_6;
				break;
			case ALGORITHM_PARITY_0:
				re->before.layout = ALGORITHM_PARITY_0_6;
				break;
			case ALGORITHM_PARITY_N:
				re->before.layout = ALGORITHM_PARITY_N_6;
				break;
			default:
				return "Cannot convert an array with this layout";
			}
			break;
		case 1:
			if (info->array.raid_disks != 2)
				return "Can only convert a 2-device array to RAID1";
			re->level = 1;
			break;
		default:
			return "Impossible level change requested";
		}
		break;
	case 6:
		switch (info->new_level) {
		case 4:
		case 5:
		case 6:
			re->level = 6;
			re->before.data_disks = info->array.raid_disks - 2;
			re->before.layout = info->array.layout;
			break;
		default:
			return "Impossible level change requested";
		}
		break;
	}

	/* If we reached here then it looks like a re-stripe is
	 * happening.  We have determined the intermediate level
	 * and initial raid_disks/layout and stored these in 're'.
	 *
	 * We need to deduce the final layout that can be atomically
	 * converted to the end state.
	 */
	switch (info->new_level) {
	case 0:
		/* We can only get to RAID0 from RAID4 or RAID5
		 * with appropriate layout and one extra device
		 */
		if (re->level != 4 && re->level != 5)
			return "Cannot covert to RAID0 from this level";
		if (info->delta_disks == UnSet)
			re->after.data_disks = re->before.data_disks;
		else
			re->after.data_disks =
				info->array.raid_disks + info->delta_disks;
		switch (re->level) {
		case 4:
			re->after.layout = 0 ; break;
		case 5:
			re->after.layout = ALGORITHM_PARITY_N; break;
		}
		break;

	case 4:
		/* We can only get to RAID4 from RAID5 */
		if (re->level != 4 && re->level != 5)
			return "Cannot convert to RAID4 from this level";
		if (info->delta_disks == UnSet)
			re->after.data_disks = re->before.data_disks;
		else
			re->after.data_disks =
				re->before.data_disks + info->delta_disks;
		switch (re->level) {
		case 4:
			re->after.layout = 0 ; break;
		case 5:
			re->after.layout = ALGORITHM_PARITY_N; break;
		}
		break;

	case 5:
		/* We get to RAID5 for RAID5 or RAID6 */
		if (re->level != 5 && re->level != 6)
			return "Cannot convert to RAID5 from this level";
		if (info->delta_disks == UnSet)
			re->after.data_disks = re->before.data_disks;
		else if (re->level == 5)
			re->after.data_disks =
				re->before.data_disks + info->delta_disks;
		else
			re->after.data_disks =
				info->array.raid_disks + info->delta_disks - 1;
		switch (re->level) {
		case 5:
			if (info->new_layout == UnSet)
				re->after.layout = re->before.layout;
			else
				re->after.layout = info->new_layout;
			break;
		case 6:
			if (info->new_layout == UnSet)
				info->new_layout = re->before.layout;

			/* after.layout needs to be raid6 version of new_layout */
			if (info->new_layout == ALGORITHM_PARITY_N)
				re->after.layout = ALGORITHM_PARITY_N;
			else {
				char layout[40];
				char *ls = map_num(r5layout, info->new_layout);
				int l;
				strcat(strcpy(layout, ls), "-6");
				l = map_name(r6layout, layout);
				if (l == UnSet)
					return "Cannot find RAID6 layout"
						" to convert to";
				re->after.layout = l;
			}
		}
		break;

	case 6:
		/* We must already be at level 6 */
		if (re->level != 6)
			return "Impossible level change";
		if (info->delta_disks == UnSet)
			re->after.data_disks = re->before.data_disks;
		else
			re->after.data_disks = (info->array.raid_disks +
						info->delta_disks) - 2;
		if (info->new_layout == UnSet)
			re->after.layout = info->array.layout;
		else
			re->after.layout = info->new_layout;
		break;
	default:
		return "Impossible level change requested";
	}
	switch (re->level) {
	case 6: re->parity = 2; break;
	case 4:
	case 5: re->parity = 1; break;
	default: re->parity = 0; break;
	}
	/* So we have a restripe operation, we need to calculate the number
	 * of blocks per reshape operation.
	 */
	if (info->new_chunk == 0)
		info->new_chunk = info->array.chunk_size;
	if (re->after.data_disks == re->before.data_disks &&
	    re->after.layout == re->before.layout &&
	    info->new_chunk == info->array.chunk_size) {
		/* Nothing to change */
		re->backup_blocks = 0;
		return NULL;
	}
	if (re->after.data_disks == 1 && re->before.data_disks == 1) {
		/* chunk and layout changes make no difference */
		re->backup_blocks = 0;
		return NULL;
	}

	if (re->after.data_disks == re->before.data_disks &&
	    get_linux_version() < 2006032)
		return "in-place reshape is not safe before 2.6.32 - sorry.";

	if (re->after.data_disks < re->before.data_disks &&
	    get_linux_version() < 2006030)
		return "reshape to fewer devices is not supported before 2.6.32 - sorry.";

	re->backup_blocks = compute_backup_blocks(
		info->new_chunk, info->array.chunk_size,
		re->after.data_disks,
		re->before.data_disks);

	re->new_size = info->component_size * re->after.data_disks;
	return NULL;
}

static int reshape_array(char *container, int fd, char *devname,
			 struct supertype *st, struct mdinfo *info,
			 int force, char *backup_file, int quiet, int forked,
			 int restart);
static int reshape_container(char *container, int cfd, char *devname,
			     struct supertype *st, 
			     struct mdinfo *info,
			     int force,
			     char *backup_file,
			     int quiet);

int Grow_reshape(char *devname, int fd, int quiet, char *backup_file,
		 long long size,
		 int level, char *layout_str, int chunksize, int raid_disks,
		 int force)
{
	/* Make some changes in the shape of an array.
	 * The kernel must support the change.
	 *
	 * There are three different changes.  Each can trigger
	 * a resync or recovery so we freeze that until we have
	 * requested everything (if kernel supports freezing - 2.6.30).
	 * The steps are:
	 *  - change size (i.e. component_size)
	 *  - change level
	 *  - change layout/chunksize/ndisks
	 *
	 * The last can require a reshape.  It is different on different
	 * levels so we need to check the level before actioning it.
	 * Some times the level change needs to be requested after the
	 * reshape (e.g. raid6->raid5, raid5->raid0)
	 *
	 */
	struct mdu_array_info_s array;
	int rv = 0;
	struct supertype *st;
	char *subarray = NULL;

	int frozen;
	int changed = 0;
	char *container = NULL;
	char container_buf[20];
	int cfd = -1;

	struct mdinfo info;
	struct mdinfo *sra;

	if (ioctl(fd, GET_ARRAY_INFO, &array) < 0) {
		fprintf(stderr, Name ": %s is not an active md array - aborting\n",
			devname);
		return 1;
	}

	if (size >= 0 &&
	    (chunksize || level!= UnSet || layout_str || raid_disks)) {
		fprintf(stderr, Name ": cannot change component size at the same time "
			"as other changes.\n"
			"   Change size first, then check data is intact before "
			"making other changes.\n");
		return 1;
	}

	if (raid_disks && raid_disks < array.raid_disks && array.level > 1 &&
	    get_linux_version() < 2006032 &&
	    !check_env("MDADM_FORCE_FEWER")) {
		fprintf(stderr, Name ": reducing the number of devices is not safe before Linux 2.6.32\n"
			"       Please use a newer kernel\n");
		return 1;
	}

	st = super_by_fd(fd, &subarray);
	if (!st) {
		fprintf(stderr, Name ": Unable to determine metadata format for %s\n", devname);
		return 1;
	}
	if (raid_disks > st->max_devs) {
		fprintf(stderr, Name ": Cannot increase raid-disks on this array"
			" beyond %d\n", st->max_devs);
		return 1;
	}

	/* in the external case we need to check that the requested reshape is
	 * supported, and perform an initial check that the container holds the
	 * pre-requisite spare devices (mdmon owns final validation)
	 */
	if (st->ss->external) {
		int container_dev;
		int rv;

		if (subarray) {
			container_dev = st->container_dev;
			cfd = open_dev_excl(st->container_dev);
		} else {
			container_dev = st->devnum;
			close(fd);
			cfd = open_dev_excl(st->devnum);
			fd = cfd;
		}
		if (cfd < 0) {
			fprintf(stderr, Name ": Unable to open container for %s\n",
				devname);
			free(subarray);
			return 1;
		}

		fmt_devname(container_buf, container_dev);
		container = container_buf;

		rv = st->ss->load_container(st, cfd, NULL);

		if (rv) {
			fprintf(stderr, Name ": Cannot read superblock for %s\n",
				devname);
			free(subarray);
			return 1;
		}

		if (mdmon_running(container_dev))
			st->update_tail = &st->updates;
	} 

	if (raid_disks > array.raid_disks &&
	    array.spare_disks < (raid_disks - array.raid_disks) &&
	    !force) {
		fprintf(stderr,
			Name ": Need %d spare%s to avoid degraded array,"
			" and only have %d.\n"
			"       Use --force to over-ride this check.\n",
			raid_disks - array.raid_disks, 
			raid_disks - array.raid_disks == 1 ? "" : "s", 
			array.spare_disks);
		return 1;
	}

	sra = sysfs_read(fd, 0, GET_LEVEL | GET_DISKS | GET_DEVS
			 | GET_STATE | GET_VERSION);
 	if (sra) {
		if (st->ss->external && subarray == NULL) {
			array.level = LEVEL_CONTAINER;
			sra->array.level = LEVEL_CONTAINER;
		}
	} else {
		fprintf(stderr, Name ": failed to read sysfs parameters for %s\n",
			devname);
		return 1;
	}
	frozen = freeze(st);
	if (frozen < -1) {
		/* freeze() already spewed the reason */
		return 1;
	} else if (frozen < 0) {
		fprintf(stderr, Name ": %s is performing resync/recovery and cannot"
			" be reshaped\n", devname);
		return 1;
	}

	/* ========= set size =============== */
	if (size >= 0 && (size == 0 || size != array.size)) {
		long long orig_size = array.size;

		if (reshape_super(st, size, UnSet, UnSet, 0, 0, NULL, devname, !quiet)) {
			rv = 1;
			goto release;
		}
		sync_metadata(st);
		array.size = size;
		if (array.size != size) {
			/* got truncated to 32bit, write to
			 * component_size instead
			 */
			if (sra)
				rv = sysfs_set_num(sra, NULL,
						   "component_size", size);
			else
				rv = -1;
		} else
			rv = ioctl(fd, SET_ARRAY_INFO, &array);
		if (rv != 0) {
			int err = errno;

			/* restore metadata */
			if (reshape_super(st, orig_size, UnSet, UnSet, 0, 0,
					  NULL, devname, !quiet) == 0)
				sync_metadata(st);
			fprintf(stderr, Name ": Cannot set device size for %s: %s\n",
				devname, strerror(err));
			if (err == EBUSY && 
			    (array.state & (1<<MD_SB_BITMAP_PRESENT)))
				fprintf(stderr, "       Bitmap must be removed before size can be changed\n");
			rv = 1;
			goto release;
		}
		ioctl(fd, GET_ARRAY_INFO, &array);
		size = get_component_size(fd)/2;
		if (size == 0)
			size = array.size;
		if (!quiet)
			fprintf(stderr, Name ": component size of %s has been set to %lluK\n",
				devname, size);
		changed = 1;
	} else if (array.level != LEVEL_CONTAINER) {
		size = get_component_size(fd)/2;
		if (size == 0)
			size = array.size;
	}

	/* ========= check for Raid10/Raid1 -> Raid0 conversion ===============
	 * current implementation assumes that following conditions must be met:
	 * - RAID10:
	 * 	- far_copies == 1
	 * 	- near_copies == 2
	 */
	if ((level == 0 && array.level == 10 && sra &&
	    array.layout == ((1 << 8) + 2) && !(array.raid_disks & 1)) ||
	    (level == 0 && array.level == 1 && sra)) {
		int err;
		err = remove_disks_for_takeover(st, sra, array.layout);
		if (err) {
			dprintf(Name": Array cannot be reshaped\n");
			if (cfd > -1)
				close(cfd);
			rv = 1;
			goto release;
		}
		/* FIXME this is added with no justification - why is it here */
		ping_monitor(container);
	}

	info.array = array;
	sysfs_init(&info, fd, NoMdDev);
	strcpy(info.text_version, sra->text_version);
	info.component_size = size*2;
	info.new_level = level;
	info.new_chunk = chunksize * 1024;
	if (raid_disks)
		info.delta_disks = raid_disks - info.array.raid_disks;
	else
		info.delta_disks = UnSet;
	if (layout_str == NULL) {
		info.new_layout = UnSet;
		if (info.array.level == 6 &&
		    (info.new_level == 6 || info.new_level == UnSet) &&
		    info.array.layout >= 16) {
			fprintf(stderr, Name
				": %s has a non-standard layout.  If you"
				" wish to preserve this\n"
				"      during the reshape, please specify"
				" --layout=preserve\n"
				"      If you want to change it, specify a"
				" layout or use --layout=normalise\n",
				devname);
			rv = 1;
			goto release;
		}
	} else if (strcmp(layout_str, "normalise") == 0 ||
		 strcmp(layout_str, "normalize") == 0) {
		/* If we have a -6 RAID6 layout, remove the '-6'. */
		info.new_layout = UnSet;
		if (info.array.level == 6 && info.new_level == UnSet) {
			char l[40], *h;
			strcpy(l, map_num(r6layout, info.array.layout));
			h = strrchr(l, '-');
			if (h && strcmp(h, "-6") == 0) {
				*h = 0;
				info.new_layout = map_name(r6layout, l);
			}
		}
	} else if (strcmp(layout_str, "preserve") == 0) {
		info.new_layout = UnSet;
	} else {
		int l = info.new_level;
		if (l == UnSet)
			l = info.array.level;
		switch (l) {
		case 5:
			info.new_layout = map_name(r5layout, layout_str);
			break;
		case 6:
			info.new_layout = map_name(r6layout, layout_str);
			break;
		case 10:
			info.new_layout = parse_layout_10(layout_str);
			break;
		case LEVEL_FAULTY:
			info.new_layout = parse_layout_faulty(layout_str);
			break;
		default:
			fprintf(stderr, Name ": layout not meaningful"
				" with this level\n");
			rv = 1;
			goto release;
		}
		if (info.new_layout == UnSet) {
			fprintf(stderr, Name ": layout %s not understood"
				" for this level\n",
				layout_str);
			rv = 1;
			goto release;
		}
	}

	if (array.level == LEVEL_CONTAINER) {
		/* This change is to be applied to every array in the
		 * container.  This is only needed when the metadata imposes
		 * restraints of the various arrays in the container.
		 * Currently we only know that IMSM requires all arrays
		 * to have the same number of devices so changing the
		 * number of devices (On-Line Capacity Expansion) must be
		 * performed at the level of the container
		 */
		rv = reshape_container(container, fd, devname, st, &info,
				       force, backup_file, quiet);
		frozen = 0;
	} else {
		/* Impose these changes on a single array.  First
		 * check that the metadata is OK with the change. */

		if (reshape_super(st, info.component_size, info.new_level,
				  info.new_layout, info.new_chunk,
				  info.array.raid_disks + info.delta_disks,
				  backup_file, devname, quiet)) {
			rv = 1;
			goto release;
		}
		sync_metadata(st);
		rv = reshape_array(container, fd, devname, st, &info, force,
				   backup_file, quiet, 0, 0);
		frozen = 0;
	}
release:
	if (frozen > 0)
		unfreeze(st);
	return rv;
}

static int reshape_array(char *container, int fd, char *devname,
			 struct supertype *st, struct mdinfo *info,
			 int force,
			 char *backup_file, int quiet, int forked,
			 int restart)
{
	struct reshape reshape;
	int spares_needed;
	char *msg;
	int orig_level = UnSet;
	int disks, odisks;

	struct mdu_array_info_s array;
	char *c;

	int *fdlist;
	unsigned long long *offsets;
	int d;
	int nrdisks;
	int err;
	unsigned long blocks;
	unsigned long cache;
	unsigned long long array_size;
	int done;
	struct mdinfo *sra = NULL;

	msg = analyse_change(info, &reshape);
	if (msg) {
		fprintf(stderr, Name ": %s\n", msg);
		goto release;
	}
	if (ioctl(fd, GET_ARRAY_INFO, &array) != 0) {
		dprintf("Canot get array information.\n");
		goto release;
	}

	if (restart) {
		/* reshape already started. just skip to monitoring the reshape */
		if (reshape.backup_blocks == 0)
			return 0;
		goto started;
	}
	spares_needed = max(reshape.before.data_disks,
			    reshape.after.data_disks)
		+ reshape.parity - array.raid_disks;

	if (!force &&
	    info->new_level > 1 &&
	    spares_needed > info->array.spare_disks) {
		fprintf(stderr,
			Name ": Need %d spare%s to avoid degraded array,"
			" and only have %d.\n"
			"       Use --force to over-ride this check.\n",
			spares_needed,
			spares_needed == 1 ? "" : "s", 
			info->array.spare_disks);
		goto release;
	}

	if (reshape.level != info->array.level) {
		char *c = map_num(pers, reshape.level);
		int err;
		if (c == NULL)
			goto release;

		err = sysfs_set_str(info, NULL, "level", c);
		if (err) {
			err = errno;
			fprintf(stderr, Name ": %s: could not set level to %s\n",
				devname, c);
			if (err == EBUSY && 
			    (info->array.state & (1<<MD_SB_BITMAP_PRESENT)))
				fprintf(stderr, "       Bitmap must be removed"
					" before level can be changed\n");
			goto release;
		}
		if (!quiet)
			fprintf(stderr, Name ": level of %s changed to %s\n",
				devname, c);	
		orig_level = info->array.level;

		if (reshape.level > 0 && st->ss->external) {
			/* make sure mdmon is aware of the new level */
			if (!mdmon_running(st->container_dev))
				start_mdmon(st->container_dev);
			ping_monitor(container);
		}
	}
	/* ->reshape_super might have chosen some spares from the
	 * container that it wants to be part of the new array.
	 * We can collect them with ->container_content and give
	 * them to the kernel.
	 */
	if (st->ss->reshape_super && st->ss->container_content) {
		char *subarray = strchr(info->text_version+1, '/')+1;
		struct mdinfo *info2 =
			st->ss->container_content(st, subarray);
		struct mdinfo *d;

		if (info2) {
			sysfs_init(info2, fd, st->devnum);
			for (d = info2->devs; d; d = d->next) {
				if (d->disk.state == 0 &&
				    d->disk.raid_disk >= 0) {
					/* This is a spare that wants to
					 * be part of the array.
					 */
					add_disk(fd, st, info2, d);
				}
			}
			sysfs_free(info2);
		}
	}

	if (reshape.backup_blocks == 0) {
		/* No restriping needed, but we might need to impose
		 * some more changes: layout, raid_disks, chunk_size
		 */
		if (info->new_layout != UnSet &&
		    info->new_layout != info->array.layout) {
			info->array.layout = info->new_layout;
			if (ioctl(fd, SET_ARRAY_INFO, &info->array) != 0) {
				fprintf(stderr, Name ": failed to set new layout\n");
				goto release;
			} else if (!quiet)
				printf("layout for %s set to %d\n",
				       devname, info->array.layout);
		}
		if (info->delta_disks != UnSet &&
		    info->delta_disks != 0) {
			info->array.raid_disks += info->delta_disks;
			if (ioctl(fd, SET_ARRAY_INFO, &info->array) != 0) {
				fprintf(stderr, Name ": failed to set raid disks\n");
				goto release;
			} else if (!quiet)
				printf("raid_disks for %s set to %d\n",
				       devname, info->array.raid_disks);
		}
		if (info->new_chunk != 0 &&
		    info->new_chunk != info->array.chunk_size) {
			if (sysfs_set_num(info, NULL,
					  "chunk_size", info->new_chunk) != 0) {
				fprintf(stderr, Name ": failed to set chunk size\n");
				goto release;
			} else if (!quiet)
				printf("chunk size for %s set to %d\n",
				       devname, info->array.chunk_size);
		}
		unfreeze(st);
		return 0;
	}

	/*
	 * There are three possibilities.
	 * 1/ The array will shrink.
	 *    We need to ensure the reshape will pause before reaching
	 *    the 'critical section'.  We also need to fork and wait for
	 *    that to happen.  When it does we 
	 *       suspend/backup/complete/unfreeze
	 *
	 * 2/ The array will not change size.
	 *    This requires that we keep a backup of a sliding window
	 *    so that we can restore data after a crash.  So we need
	 *    to fork and monitor progress.
	 *    In future we will allow the data_offset to change, so
	 *    a sliding backup becomes unnecessary.
	 *
	 * 3/ The array will grow. This is relatively easy.
	 *    However the kernel's restripe routines will cheerfully
	 *    overwrite some early data before it is safe.  So we
	 *    need to make a backup of the early parts of the array
	 *    and be ready to restore it if rebuild aborts very early.
	 *    For externally managed metadata, we still need a forked
	 *    child to monitor the reshape and suspend IO over the region
	 *    that is being reshaped.
	 *
	 *    We backup data by writing it to one spare, or to a
	 *    file which was given on command line.
	 *
	 * In each case, we first make sure that storage is available
	 * for the required backup.
	 * Then we:
	 *   -  request the shape change.
	 *   -  fork to handle backup etc.
	 */
started:
	/* Check that we can hold all the data */
	get_dev_size(fd, NULL, &array_size);
	if (reshape.new_size < (array_size/512)) {
		fprintf(stderr,
			Name ": this change will reduce the size of the array.\n"
			"       use --grow --array-size first to truncate array.\n"
			"       e.g. mdadm --grow %s --array-size %llu\n",
			devname, reshape.new_size/2);
		goto release;
	}

	sra = sysfs_read(fd, 0,
			 GET_COMPONENT|GET_DEVS|GET_OFFSET|GET_STATE|GET_CHUNK|
			 GET_CACHE);
	if (!sra) {
		fprintf(stderr, Name ": %s: Cannot get array details from sysfs\n",
			devname);
		goto release;
	}

	/* Decide how many blocks (sectors) for a reshape
	 * unit.  The number we have so far is just a minimum
	 */
	blocks = reshape.backup_blocks;
	if (reshape.before.data_disks == 
	    reshape.after.data_disks) {
		/* Make 'blocks' bigger for better throughput, but
		 * not so big that we reject it below.
		 * Try for 16 megabytes
		 */
		while (blocks * 32 < sra->component_size &&
		       blocks < 16*1024*2)
			blocks *= 2;
	} else
		fprintf(stderr, Name ": Need to backup %luK of critical "
			"section..\n", blocks/2);

	if (blocks >= sra->component_size/2) {
		fprintf(stderr, Name ": %s: Something wrong"
			" - reshape aborted\n",
			devname);
		goto release;
	}

	/* Now we need to open all these devices so we can read/write.
	 */
	nrdisks = array.raid_disks + sra->array.spare_disks;
	fdlist = malloc((1+nrdisks) * sizeof(int));
	offsets = malloc((1+nrdisks) * sizeof(offsets[0]));
	if (!fdlist || !offsets) {
		fprintf(stderr, Name ": malloc failed: grow aborted\n");
		goto release;
	}

	d = reshape_prepare_fdlist(devname, sra, array.raid_disks,
				   nrdisks, blocks, backup_file,
				   fdlist, offsets);
	if (d < 0) {
		goto release;
	}
	if (backup_file == NULL) {
		if (reshape.after.data_disks <= reshape.before.data_disks) {
			fprintf(stderr,
				Name ": %s: Cannot grow - need backup-file\n", 
				devname);
			goto release;
		} else if (sra->array.spare_disks == 0) {
			fprintf(stderr, Name ": %s: Cannot grow - need a spare or "
				"backup-file to backup critical section\n",
				devname);
			goto release;
		}
	} else {
		if (!reshape_open_backup_file(backup_file, fd, devname,
					      (signed)blocks,
					      fdlist+d, offsets+d, restart)) {
			goto release;
		}
		d++;
	}

	/* lastly, check that the internal stripe cache is
	 * large enough, or it won't work.
	 * It must hold at least 4 stripes of the larger
	 * chunk size
	 */
	cache = max(info->array.chunk_size, info->new_chunk);
	cache *= 4; /* 4 stripes minimum */
	cache /= 512; /* convert to sectors */
	disks = min(reshape.before.data_disks, reshape.after.data_disks);
	/* make sure there is room for 'blocks' with a bit to spare */
	if (cache < 16 + blocks / disks)
		cache = 16 + blocks / disks;
	cache /= (4096/512); /* Covert from sectors to pages */

	if (sra->cache_size < cache)
		subarray_set_num(container, sra, "stripe_cache_size",
				 cache+1);

	/* Right, everything seems fine. Let's kick things off.
	 * If only changing raid_disks, use ioctl, else use
	 * sysfs.
	 */
	sync_metadata(st);

	sra->new_chunk = info->new_chunk;

	if (info->reshape_active)
		sra->reshape_progress = info->reshape_progress;
	else {
		sra->reshape_progress = 0;
		if (reshape.after.data_disks < reshape.before.data_disks)
			/* start from the end of the new array */
			sra->reshape_progress = (sra->component_size
						 * reshape.after.data_disks);
	}

	if (info->array.chunk_size == info->new_chunk &&
	    reshape.before.layout == reshape.after.layout &&
	    st->ss->external == 0) {
		/* use SET_ARRAY_INFO but only if reshape hasn't started */
		array.raid_disks = reshape.after.data_disks + reshape.parity;
		if (!info->reshape_active &&
		    ioctl(fd, SET_ARRAY_INFO, &array) != 0) {
			int err = errno;

			fprintf(stderr,
				Name ": Cannot set device shape for %s: %s\n",
				devname, strerror(errno));

			if (err == EBUSY && 
			    (array.state & (1<<MD_SB_BITMAP_PRESENT)))
				fprintf(stderr,
					"       Bitmap must be removed before"
					" shape can be changed\n");

			goto release;
		}
	} else {
		/* set them all just in case some old 'new_*' value
		 * persists from some earlier problem.
		 * We even set them when restarting in the middle.  They will
		 * already be set in that case so this will be a no-op,
		 * but it is hard to tell the difference.
		 */
		int err = 0;
		if (sysfs_set_num(sra, NULL, "chunk_size", info->new_chunk) < 0)
			err = errno;
		if (!err && sysfs_set_num(sra, NULL, "layout", 
					 reshape.after.layout) < 0)
			err = errno;
		if (!err && subarray_set_num(container, sra, "raid_disks",
					    reshape.after.data_disks +
					    reshape.parity) < 0)
			err = errno;
		if (err) {
			fprintf(stderr, Name ": Cannot set device shape for %s\n",
				devname);

			if (err == EBUSY && 
			    (array.state & (1<<MD_SB_BITMAP_PRESENT)))
				fprintf(stderr,
					"       Bitmap must be removed before"
					" shape can be changed\n");
			goto release;
		}
	}

	start_reshape(sra);
	if (restart)
		sysfs_set_str(sra, NULL, "array_state", "active");

	/* Now we just need to kick off the reshape and watch, while
	 * handling backups of the data...
	 * This is all done by a forked background process.
	 */
	switch(forked ? 0 : fork()) {
	case -1:
		fprintf(stderr, Name ": Cannot run child to monitor reshape: %s\n",
			strerror(errno));
		abort_reshape(sra);
		goto release;
	default:
		return 0;
	case 0:
		break;
	}

	close(fd);
	if (check_env("MDADM_GROW_VERIFY"))
		fd = open(devname, O_RDONLY | O_DIRECT);
	else
		fd = -1;
	mlockall(MCL_FUTURE);

	odisks = reshape.before.data_disks + reshape.parity;

	if (st->ss->external) {
		/* metadata handler takes it from here */
		done = st->ss->manage_reshape(
			fd, sra, &reshape, st, blocks,
			fdlist, offsets,
			d - odisks, fdlist+odisks,
			offsets+odisks);
	} else
		done = child_monitor(
			fd, sra, &reshape, st, blocks,
			fdlist, offsets,
			d - odisks, fdlist+odisks,
			offsets+odisks);

	if (backup_file && done)
		unlink(backup_file);
	if (!done) {
		abort_reshape(sra);
		goto out;
	}

	if (!st->ss->external &&
	    !(reshape.before.data_disks != reshape.after.data_disks
	      && info->custom_array_size) &&
	    info->new_level == reshape.level &&
	    !forked) {
		/* no need to wait for the reshape to finish as
		 * there is nothing more to do.
		 */
		exit(0);
	}
	wait_reshape(sra);

	if (st->ss->external) {
		/* Re-load the metadata as much could have changed */
		int cfd = open_dev(st->container_dev);
		if (cfd >= 0) {
			ping_monitor(container);
			st->ss->free_super(st);
			st->ss->load_container(st, cfd, container);
			close(cfd);
		}
	}

	/* set new array size if required customer_array_size is used
	 * by this metadata.
	 */
	if (reshape.before.data_disks !=
	    reshape.after.data_disks &&
	    info->custom_array_size) {
		struct mdinfo *info2;
		char *subarray = strchr(info->text_version+1, '/')+1;

		info2 = st->ss->container_content(st, subarray);
		if (info2) {
			unsigned long long current_size = 0;
			unsigned long long new_size =
				info2->custom_array_size/2;

			if (sysfs_get_ll(sra,
					 NULL,
					 "array_size",
					 &current_size) == 0 &&
			    new_size > current_size) {
				if (sysfs_set_num(sra, NULL,
						  "array_size", new_size)
				    < 0)
					dprintf("Error: Cannot"
						" set array size");
				else
					dprintf("Array size "
						"changed");
				dprintf(" from %llu to %llu.\n",
					current_size, new_size);
			}
			sysfs_free(info2);
		}
	}

	if (info->new_level != reshape.level) {

		c = map_num(pers, info->new_level);
		if (c) {
			err = sysfs_set_str(sra, NULL, "level", c);
			if (err)
				fprintf(stderr, Name\
					": %s: could not set level "
					"to %s\n", devname, c);
		}
	}
out:
	if (forked)
		return 0;
	exit(0);

release:
	if (orig_level != UnSet && sra) {
		c = map_num(pers, orig_level);
		if (c && sysfs_set_str(sra, NULL, "level", c) == 0)
			fprintf(stderr, Name ": aborting level change\n");
	}
	if (!forked)
		unfreeze(st);
	return 1;
}

int reshape_container(char *container, int cfd, char *devname,
		      struct supertype *st, 
		      struct mdinfo *info,
		      int force,
		      char *backup_file,
		      int quiet)
{
	struct mdinfo *cc = NULL;

	/* component_size is not meaningful for a container,
	 * so pass '-1' meaning 'no change'
	 */
	if (reshape_super(st, -1, info->new_level,
			  info->new_layout, info->new_chunk,
			  info->array.raid_disks + info->delta_disks,
			  backup_file, devname, quiet))
		return 1;

	sync_metadata(st);

	/* ping monitor to be sure that update is on disk
	 */
	ping_monitor(container);

	switch (fork()) {
	case -1: /* error */
		perror("Cannot fork to complete reshape\n");
		return 1;
	default: /* parent */
		printf(Name ": multi-array reshape continues in background\n");
		return 0;
	case 0: /* child */
		break;
	}

	while(1) {
		/* For each member array with reshape_active,
		 * we need to perform the reshape.
		 * We pick the first array that needs reshaping and
		 * reshape it.  reshape_array() will re-read the metadata
		 * so the next time through a different array should be
		 * ready for reshape.
		 */
		struct mdinfo *content;
		int rv;
		int fd;
		struct mdstat_ent *mdstat;
		char *adev;

		sysfs_free(cc);

		cc = st->ss->container_content(st, NULL);

		for (content = cc; content ; content = content->next) {
			char *subarray;
			if (!content->reshape_active)
				continue;

			subarray = strchr(content->text_version+1, '/')+1;
			mdstat = mdstat_by_subdev(subarray,
						  devname2devnum(container));
			if (!mdstat)
				continue;
			break;
		}
		if (!content)
			break;

		fd = open_dev(mdstat->devnum);
		if (fd < 0)
			break;
		adev = map_dev(dev2major(mdstat->devnum),
			       dev2minor(mdstat->devnum),
			       0);
		if (!adev)
			adev = content->text_version;

		sysfs_init(content, fd, mdstat->devnum);

		rv = reshape_array(container, fd, adev, st,
				   content, force,
				   backup_file, quiet, 1, 0);
		close(fd);
		if (rv)
			break;
	}
	unfreeze(st);
	sysfs_free(cc);
	exit(0);
}

/*
 * We run a child process in the background which performs the following
 * steps:
 *   - wait for resync to reach a certain point
 *   - suspend io to the following section
 *   - backup that section
 *   - allow resync to proceed further
 *   - resume io
 *   - discard the backup.
 *
 * When are combined in slightly different ways in the three cases.
 * Grow:
 *   - suspend/backup/allow/wait/resume/discard
 * Shrink:
 *   - allow/wait/suspend/backup/allow/wait/resume/discard
 * same-size:
 *   - wait/resume/discard/suspend/backup/allow
 *
 * suspend/backup/allow always come together
 * wait/resume/discard do too.
 * For the same-size case we have two backups to improve flow.
 * 
 */

int progress_reshape(struct mdinfo *info, struct reshape *reshape,
		     unsigned long long backup_point,
		     unsigned long long wait_point,
		     unsigned long long *suspend_point,
		     unsigned long long *reshape_completed)
{
	/* This function is called repeatedly by the reshape manager.
	 * It determines how much progress can safely be made and allows
	 * that progress.
	 * - 'info' identifies the array and particularly records in
	 *    ->reshape_progress the metadata's knowledge of progress
	 *      This is a sector offset from the start of the array
	 *      of the next array block to be relocated.  This number
	 *      may increase from 0 or decrease from array_size, depending
	 *      on the type of reshape that is happening.
	 *    Note that in contrast, 'sync_completed' is a block count of the
	 *    reshape so far.  It gives the distance between the start point
	 *    (head or tail of device) and the next place that data will be
	 *    written.  It always increases.
	 * - 'reshape' is the structure created by analyse_change
	 * - 'backup_point' shows how much the metadata manager has backed-up
	 *   data.  For reshapes with increasing progress, it is the next address
	 *   to be backed up, previous addresses have been backed-up.  For
	 *   decreasing progress, it is the earliest address that has been
	 *   backed up - later address are also backed up.
	 *   So addresses between reshape_progress and backup_point are
	 *   backed up providing those are in the 'correct' order.
	 * - 'wait_point' is an array address.  When reshape_completed
	 *   passes this point, progress_reshape should return.  It might
	 *   return earlier if it determines that ->reshape_progress needs
	 *   to be updated or further backup is needed.
	 * - suspend_point is maintained by progress_reshape and the caller
	 *   should not touch it except to initialise to zero.
	 *   It is an array address and it only increases in 2.6.37 and earlier.
	 *   This makes it difficult to handle reducing reshapes with
	 *   external metadata.
	 *   However:  it is similar to backup_point in that it records the
	 *     other end of a suspended region from  reshape_progress.
	 *     it is moved to extend the region that is safe to backup and/or
	 *     reshape
	 * - reshape_completed is read from sysfs and returned.  The caller
	 *   should copy this into ->reshape_progress when it has reason to
	 *   believe that the metadata knows this, and any backup outside this
	 *   has been erased.
	 *
	 * Return value is:
	 *   1 if more data from backup_point - but only as far as suspend_point,
	 *     should be backed up
	 *   0 if things are progressing smoothly
	 *  -1 if the reshape is finished, either because it is all done,
	 *     or due to an error.
	 */

	int advancing = (reshape->after.data_disks
			 >= reshape->before.data_disks);
	unsigned long long need_backup; /* All data between start of array and
					 * here will at some point need to
					 * be backed up.
					 */
	unsigned long long read_offset, write_offset;
	unsigned long long write_range;
	unsigned long long max_progress, target, completed;
	unsigned long long array_size = (info->component_size
					 * reshape->before.data_disks);
	int fd;
	char buf[20];

	/* First, we unsuspend any region that is now known to be safe.
	 * If suspend_point is on the 'wrong' side of reshape_progress, then
	 * we don't have or need suspension at the moment.  This is true for
	 * native metadata when we don't need to back-up.
	 */
	if (advancing) {
		if (info->reshape_progress <= *suspend_point)
			sysfs_set_num(info, NULL, "suspend_lo",
				      info->reshape_progress);
	} else {
		/* Note: this won't work in 2.6.37 and before.
		 * Something somewhere should make sure we don't need it!
		 */
		if (info->reshape_progress >= *suspend_point)
			sysfs_set_num(info, NULL, "suspend_hi",
				      info->reshape_progress);
	}

	/* Now work out how far it is safe to progress.
	 * If the read_offset for ->reshape_progress is less than
	 * 'blocks' beyond the write_offset, we can only progress as far
	 * as a backup.
	 * Otherwise we can progress until the write_offset for the new location
	 * reaches (within 'blocks' of) the read_offset at the current location.
	 * However that region must be suspended unless we are using native
	 * metadata.
	 * If we need to suspend more, we limit it to 128M per device, which is
	 * rather arbitrary and should be some time-based calculation.
	 */
	read_offset = info->reshape_progress / reshape->before.data_disks;
	write_offset = info->reshape_progress / reshape->after.data_disks;
	write_range = info->new_chunk/512;
	if (reshape->before.data_disks == reshape->after.data_disks)
		need_backup = array_size;
	else
		need_backup = reshape->backup_blocks;
	if (advancing) {
		if (read_offset < write_offset + write_range)
			max_progress = backup_point;
		else
			max_progress =
				read_offset *
				reshape->after.data_disks;
	} else {
		if (read_offset > write_offset - write_range)
			/* Can only progress as far as has been backed up,
			 * which must be suspended */
			max_progress = backup_point;
		else if (info->reshape_progress <= need_backup)
			max_progress = backup_point;
		else {
			if (info->array.major_version >= 0)
				/* Can progress until backup is needed */
				max_progress = need_backup;
			else {
				/* Can progress until metadata update is required */
				max_progress =
					read_offset *
					reshape->after.data_disks;
				/* but data must be suspended */
				if (max_progress < *suspend_point)
					max_progress = *suspend_point;
			}
		}
	}

	/* We know it is safe to progress to 'max_progress' providing
	 * it is suspended or we are using native metadata.
	 * Consider extending suspend_point 128M per device if it
	 * is less than 64M per device beyond reshape_progress.
	 * But always do a multiple of 'blocks'
	 * FIXME this is too big - it takes to long to complete
	 * this much.
	 */
	target = 64*1024*2 * min(reshape->before.data_disks,
				  reshape->after.data_disks);
	target /= reshape->backup_blocks;
	if (target < 2)
		target = 2;
	target *= reshape->backup_blocks;

	/* For externally managed metadata we always need to suspend IO to
	 * the area being reshaped so we regularly push suspend_point forward.
	 * For native metadata we only need the suspend if we are going to do
	 * a backup.
	 */
	if (advancing) {
		if ((need_backup > info->reshape_progress
		     || info->array.major_version < 0) &&
		    *suspend_point < info->reshape_progress + target) {
			if (need_backup < *suspend_point + 2 * target)
				*suspend_point = need_backup;
			else if (*suspend_point + 2 * target < array_size)
				*suspend_point += 2 * target;
			else
				*suspend_point = array_size;
			sysfs_set_num(info, NULL, "suspend_hi", *suspend_point);
			if (max_progress > *suspend_point)
				max_progress = *suspend_point;
		}
	} else {
		if (info->array.major_version >= 0) {
			/* Only need to suspend when about to backup */
			if (info->reshape_progress < need_backup * 2 &&
			    *suspend_point > 0) {
				*suspend_point = 0;
				sysfs_set_num(info, NULL, "suspend_lo", 0);
				sysfs_set_num(info, NULL, "suspend_hi", need_backup);
			}
		} else {
			/* Need to suspend continually */
			if (info->reshape_progress < *suspend_point)
				*suspend_point = info->reshape_progress;
			if (*suspend_point + target < info->reshape_progress)
				/* No need to move suspend region yet */;
			else {
				if (*suspend_point >= 2 * target)
					*suspend_point -= 2 * target;
				else
					*suspend_point = 0;
				sysfs_set_num(info, NULL, "suspend_lo",
					      *suspend_point);
			}
			if (max_progress < *suspend_point)
				max_progress = *suspend_point;
		}
	}

	/* now set sync_max to allow that progress. sync_max, like
	 * sync_completed is a count of sectors written per device, so
	 * we find the difference between max_progress and the start point,
	 * and divide that by after.data_disks to get a sync_max
	 * number.
	 * At the same time we convert wait_point to a similar number
	 * for comparing against sync_completed.
	 */
	/* scale down max_progress to per_disk */
	max_progress /= reshape->after.data_disks;
	/* Round to chunk size as some kernels give an erroneously high number */
	max_progress /= info->new_chunk/512;
	max_progress *= info->new_chunk/512;
	/* Limit progress to the whole device */
	if (max_progress > info->component_size)
		max_progress = info->component_size;
	wait_point /= reshape->after.data_disks;
	if (!advancing) {
		/* switch from 'device offset' to 'processed block count' */
		max_progress = info->component_size - max_progress;
		wait_point = info->component_size - wait_point;
	}

	sysfs_set_num(info, NULL, "sync_max", max_progress);

	/* Now wait.  If we have already reached the point that we were
	 * asked to wait to, don't wait at all, else wait for any change.
	 * We need to select on 'sync_completed' as that is the place that
	 * notifications happen, but we are really interested in
	 * 'reshape_position'
	 */
	fd = sysfs_get_fd(info, NULL, "sync_completed");
	if (fd < 0)
		goto check_progress;

	if (sysfs_fd_get_ll(fd, &completed) < 0) {
		close(fd);
		goto check_progress;
	}
	while (completed < max_progress && completed < wait_point) {
		/* Check that sync_action is still 'reshape' to avoid
		 * waiting forever on a dead array
		 */
		char action[20];
		fd_set rfds;
		if (sysfs_get_str(info, NULL, "sync_action",
				  action, 20) <= 0 ||
		    strncmp(action, "reshape", 7) != 0)
			break;
		/* Some kernels reset 'sync_completed' to zero
		 * before setting 'sync_action' to 'idle'.
		 * So we need these extra tests.
		 */
		if (completed == 0 && advancing
		    && info->reshape_progress > 0)
			break;
		if (completed == 0 && !advancing
		    && info->reshape_progress < (info->component_size
						 * reshape->after.data_disks))
			break;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		select(fd+1, NULL, NULL, &rfds, NULL);
		if (sysfs_fd_get_ll(fd, &completed) < 0) {
			close(fd);
			goto check_progress;
		}
	}
	/* some kernels can give an incorrectly high 'completed' number */
	completed /= (info->new_chunk/512);
	completed *= (info->new_chunk/512);
	/* Convert 'completed' back in to a 'progress' number */
	completed *= reshape->after.data_disks;
	if (!advancing) {
		completed = info->component_size * reshape->after.data_disks
			- completed;
	}
	*reshape_completed = completed;
	
	close(fd);

	/* We return the need_backup flag.  Caller will decide
	 * how much - a multiple of ->backup_blocks up to *suspend_point
	 */
	if (advancing)
		return need_backup > info->reshape_progress;
	else
		return need_backup >= info->reshape_progress;

check_progress:
	/* if we couldn't read a number from sync_completed, then
	 * either the reshape did complete, or it aborted.
	 * We can tell which by checking for 'none' in reshape_position.
	 */
	strcpy(buf, "hi");
	if (sysfs_get_str(info, NULL, "reshape_position", buf, sizeof(buf)) < 0
	    || strncmp(buf, "none", 4) != 0)
		return -2; /* abort */
	else {
		/* Maybe racing with array shutdown - check state */
		if (sysfs_get_str(info, NULL, "array_state", buf, sizeof(buf)) < 0
		    || strncmp(buf, "inactive", 8) == 0
		    || strncmp(buf, "clear",5) == 0)
			return -2; /* abort */
		return -1; /* complete */
	}
}


/* FIXME return status is never checked */
static int grow_backup(struct mdinfo *sra,
		unsigned long long offset, /* per device */
		unsigned long stripes, /* per device, in old chunks */
		int *sources, unsigned long long *offsets,
		int disks, int chunk, int level, int layout,
		int dests, int *destfd, unsigned long long *destoffsets,
		int part, int *degraded,
		char *buf)
{
	/* Backup 'blocks' sectors at 'offset' on each device of the array,
	 * to storage 'destfd' (offset 'destoffsets'), after first
	 * suspending IO.  Then allow resync to continue
	 * over the suspended section.
	 * Use part 'part' of the backup-super-block.
	 */
	int odata = disks;
	int rv = 0;
	int i;
	unsigned long long ll;
	int new_degraded;
	//printf("offset %llu\n", offset);
	if (level >= 4)
		odata--;
	if (level == 6)
		odata--;

	/* Check that array hasn't become degraded, else we might backup the wrong data */
	if (sysfs_get_ll(sra, NULL, "degraded", &ll) < 0)
		return -1; /* FIXME this error is ignored */
	new_degraded = (int)ll;
	if (new_degraded != *degraded) {
		/* check each device to ensure it is still working */
		struct mdinfo *sd;
		for (sd = sra->devs ; sd ; sd = sd->next) {
			if (sd->disk.state & (1<<MD_DISK_FAULTY))
				continue;
			if (sd->disk.state & (1<<MD_DISK_SYNC)) {
				char sbuf[20];
				if (sysfs_get_str(sra, sd, "state", sbuf, 20) < 0 ||
				    strstr(sbuf, "faulty") ||
				    strstr(sbuf, "in_sync") == NULL) {
					/* this device is dead */
					sd->disk.state = (1<<MD_DISK_FAULTY);
					if (sd->disk.raid_disk >= 0 &&
					    sources[sd->disk.raid_disk] >= 0) {
						close(sources[sd->disk.raid_disk]);
						sources[sd->disk.raid_disk] = -1;
					}
				}
			}
		}
		*degraded = new_degraded;
	}
	if (part) {
		bsb.arraystart2 = __cpu_to_le64(offset * odata);
		bsb.length2 = __cpu_to_le64(stripes * (chunk/512) * odata);
	} else {
		bsb.arraystart = __cpu_to_le64(offset * odata);
		bsb.length = __cpu_to_le64(stripes * (chunk/512) * odata);
	}
	if (part)
		bsb.magic[15] = '2';
	for (i = 0; i < dests; i++)
		if (part)
			lseek64(destfd[i], destoffsets[i] + __le64_to_cpu(bsb.devstart2)*512, 0);
		else
			lseek64(destfd[i], destoffsets[i], 0);

	rv = save_stripes(sources, offsets, 
			  disks, chunk, level, layout,
			  dests, destfd,
			  offset*512*odata, stripes * chunk * odata,
			  buf);

	if (rv)
		return rv;
	bsb.mtime = __cpu_to_le64(time(0));
	for (i = 0; i < dests; i++) {
		bsb.devstart = __cpu_to_le64(destoffsets[i]/512);

		bsb.sb_csum = bsb_csum((char*)&bsb, ((char*)&bsb.sb_csum)-((char*)&bsb));
		if (memcmp(bsb.magic, "md_backup_data-2", 16) == 0)
			bsb.sb_csum2 = bsb_csum((char*)&bsb,
						((char*)&bsb.sb_csum2)-((char*)&bsb));

		rv = -1;
		if ((unsigned long long)lseek64(destfd[i], destoffsets[i] - 4096, 0)
		    != destoffsets[i] - 4096)
			break;
		if (write(destfd[i], &bsb, 512) != 512)
			break;
		if (destoffsets[i] > 4096) {
			if ((unsigned long long)lseek64(destfd[i], destoffsets[i]+stripes*chunk*odata, 0) !=
			    destoffsets[i]+stripes*chunk*odata)
				break;
			if (write(destfd[i], &bsb, 512) != 512)
				break;
		}
		fsync(destfd[i]);
		rv = 0;
	}

	return rv;
}

/* in 2.6.30, the value reported by sync_completed can be
 * less that it should be by one stripe.
 * This only happens when reshape hits sync_max and pauses.
 * So allow wait_backup to either extent sync_max further
 * than strictly necessary, or return before the
 * sync has got quite as far as we would really like.
 * This is what 'blocks2' is for.
 * The various caller give appropriate values so that
 * every works.
 */
/* FIXME return value is often ignored */
static int forget_backup(
		int dests, int *destfd, unsigned long long *destoffsets,
		int part)
{
	/* 
	 * Erase backup 'part' (which is 0 or 1)
	 */
	int i;
	int rv;

	if (part) {
		bsb.arraystart2 = __cpu_to_le64(0);
		bsb.length2 = __cpu_to_le64(0);
	} else {
		bsb.arraystart = __cpu_to_le64(0);
		bsb.length = __cpu_to_le64(0);
	}
	bsb.mtime = __cpu_to_le64(time(0));
	rv = 0;
	for (i = 0; i < dests; i++) {
		bsb.devstart = __cpu_to_le64(destoffsets[i]/512);
		bsb.sb_csum = bsb_csum((char*)&bsb, ((char*)&bsb.sb_csum)-((char*)&bsb));
		if (memcmp(bsb.magic, "md_backup_data-2", 16) == 0)
			bsb.sb_csum2 = bsb_csum((char*)&bsb,
						((char*)&bsb.sb_csum2)-((char*)&bsb));
		if ((unsigned long long)lseek64(destfd[i], destoffsets[i]-4096, 0) !=
		    destoffsets[i]-4096)
			rv = -1;
		if (rv == 0 && 
		    write(destfd[i], &bsb, 512) != 512)
			rv = -1;
		fsync(destfd[i]);
	}
	return rv;
}

static void fail(char *msg)
{
	int rv;
	rv = (write(2, msg, strlen(msg)) != (int)strlen(msg));
	rv |= (write(2, "\n", 1) != 1);
	exit(rv ? 1 : 2);
}

static char *abuf, *bbuf;
static unsigned long long abuflen;
static void validate(int afd, int bfd, unsigned long long offset)
{
	/* check that the data in the backup against the array.
	 * This is only used for regression testing and should not
	 * be used while the array is active
	 */
	if (afd < 0)
		return;
	lseek64(bfd, offset - 4096, 0);
	if (read(bfd, &bsb2, 512) != 512)
		fail("cannot read bsb");
	if (bsb2.sb_csum != bsb_csum((char*)&bsb2,
				     ((char*)&bsb2.sb_csum)-((char*)&bsb2)))
		fail("first csum bad");
	if (memcmp(bsb2.magic, "md_backup_data", 14) != 0)
		fail("magic is bad");
	if (memcmp(bsb2.magic, "md_backup_data-2", 16) == 0 &&
	    bsb2.sb_csum2 != bsb_csum((char*)&bsb2,
				     ((char*)&bsb2.sb_csum2)-((char*)&bsb2)))
		fail("second csum bad");

	if (__le64_to_cpu(bsb2.devstart)*512 != offset)
		fail("devstart is wrong");

	if (bsb2.length) {
		unsigned long long len = __le64_to_cpu(bsb2.length)*512;

		if (abuflen < len) {
			free(abuf);
			free(bbuf);
			abuflen = len;
			if (posix_memalign((void**)&abuf, 4096, abuflen) ||
			    posix_memalign((void**)&bbuf, 4096, abuflen)) {
				abuflen = 0;
				/* just stop validating on mem-alloc failure */
				return;
			}
		}

		lseek64(bfd, offset, 0);
		if ((unsigned long long)read(bfd, bbuf, len) != len) {
			//printf("len %llu\n", len);
			fail("read first backup failed");
		}
		lseek64(afd, __le64_to_cpu(bsb2.arraystart)*512, 0);
		if ((unsigned long long)read(afd, abuf, len) != len)
			fail("read first from array failed");
		if (memcmp(bbuf, abuf, len) != 0) {
			#if 0
			int i;
			printf("offset=%llu len=%llu\n",
			       (unsigned long long)__le64_to_cpu(bsb2.arraystart)*512, len);
			for (i=0; i<len; i++)
				if (bbuf[i] != abuf[i]) {
					printf("first diff byte %d\n", i);
					break;
				}
			#endif
			fail("data1 compare failed");
		}
	}
	if (bsb2.length2) {
		unsigned long long len = __le64_to_cpu(bsb2.length2)*512;

		if (abuflen < len) {
			free(abuf);
			free(bbuf);
			abuflen = len;
			abuf = malloc(abuflen);
			bbuf = malloc(abuflen);
		}

		lseek64(bfd, offset+__le64_to_cpu(bsb2.devstart2)*512, 0);
		if ((unsigned long long)read(bfd, bbuf, len) != len)
			fail("read second backup failed");
		lseek64(afd, __le64_to_cpu(bsb2.arraystart2)*512, 0);
		if ((unsigned long long)read(afd, abuf, len) != len)
			fail("read second from array failed");
		if (memcmp(bbuf, abuf, len) != 0)
			fail("data2 compare failed");
	}
}

int child_monitor(int afd, struct mdinfo *sra, struct reshape *reshape,
		  struct supertype *st, unsigned long blocks,
		  int *fds, unsigned long long *offsets,
		  int dests, int *destfd, unsigned long long *destoffsets)
{
	/* Monitor a reshape where backup is being performed using
	 * 'native' mechanism - either to a backup file, or
	 * to some space in a spare.
	 */
	char *buf;
	int degraded = -1;
	unsigned long long speed;
	unsigned long long suspend_point, array_size;
	unsigned long long backup_point, wait_point;
	unsigned long long reshape_completed;
	int done = 0;
	int increasing = reshape->after.data_disks >= reshape->before.data_disks;
	int part = 0; /* The next part of the backup area to fill.  It may already
		       * be full, so we need to check */
	int level = reshape->level;
	int layout = reshape->before.layout;
	int data = reshape->before.data_disks;
	int disks = reshape->before.data_disks + reshape->parity;
	int chunk = sra->array.chunk_size;
	struct mdinfo *sd;
	unsigned long stripes;

	/* set up the backup-super-block.  This requires the
	 * uuid from the array.
	 */
	/* Find a superblock */
	for (sd = sra->devs; sd; sd = sd->next) {
		char *dn;
		int devfd;
		int ok;
		if (sd->disk.state & (1<<MD_DISK_FAULTY))
			continue;
		dn = map_dev(sd->disk.major, sd->disk.minor, 1);
		devfd = dev_open(dn, O_RDONLY);
		if (devfd < 0)
			continue;
		ok = st->ss->load_super(st, devfd, NULL);
		close(devfd);
		if (ok >= 0)
			break;
	}
	if (!sd) {
		fprintf(stderr, Name ": Cannot find a superblock\n");
		return 0;
	}

	memset(&bsb, 0, 512);
	memcpy(bsb.magic, "md_backup_data-1", 16);
	st->ss->uuid_from_super(st, (int*)&bsb.set_uuid);
	bsb.mtime = __cpu_to_le64(time(0));
	bsb.devstart2 = blocks;

	stripes = blocks / (sra->array.chunk_size/512) /
		reshape->before.data_disks;

	if (posix_memalign((void**)&buf, 4096, disks * chunk))
		/* Don't start the 'reshape' */
		return 0;
	if (reshape->before.data_disks == reshape->after.data_disks) {
		sysfs_get_ll(sra, NULL, "sync_speed_min", &speed);
		sysfs_set_num(sra, NULL, "sync_speed_min", 200000);
	}

	if (increasing) {
		array_size = sra->component_size * reshape->after.data_disks;
		backup_point = sra->reshape_progress;
		suspend_point = 0;
	} else {
		array_size = sra->component_size * reshape->before.data_disks;
		backup_point = reshape->backup_blocks;
		suspend_point = array_size;
	}

	while (!done) {
		int rv;

		/* Want to return as soon the oldest backup slot can
		 * be released as that allows us to start backing up
		 * some more, providing suspend_point has been
		 * advanced, which it should have.
		 */
		if (increasing) {
			wait_point = array_size;
			if (part == 0 && __le64_to_cpu(bsb.length) > 0)
				wait_point = (__le64_to_cpu(bsb.arraystart) +
					      __le64_to_cpu(bsb.length));
			if (part == 1 && __le64_to_cpu(bsb.length2) > 0)
				wait_point = (__le64_to_cpu(bsb.arraystart2) +
					      __le64_to_cpu(bsb.length2));
		} else {
			wait_point = 0;
			if (part == 0 && __le64_to_cpu(bsb.length) > 0)
				wait_point = __le64_to_cpu(bsb.arraystart);
			if (part == 1 && __le64_to_cpu(bsb.length2) > 0)
				wait_point = __le64_to_cpu(bsb.arraystart2);
		}

		rv = progress_reshape(sra, reshape,
				      backup_point, wait_point,
				      &suspend_point, &reshape_completed);
		/* external metadata would need to ping_monitor here */
		sra->reshape_progress = reshape_completed;

		/* Clear any backup region that is before 'here' */
		if (increasing) {
			if (reshape_completed >= (__le64_to_cpu(bsb.arraystart) +
						  __le64_to_cpu(bsb.length)))
				forget_backup(dests, destfd,
					      destoffsets, 0);
			if (reshape_completed >= (__le64_to_cpu(bsb.arraystart2) +
						  __le64_to_cpu(bsb.length2)))
				forget_backup(dests, destfd,
					      destoffsets, 1);
		} else {
			if (reshape_completed <= (__le64_to_cpu(bsb.arraystart)))
				forget_backup(dests, destfd,
					      destoffsets, 0);
			if (reshape_completed <= (__le64_to_cpu(bsb.arraystart2)))
				forget_backup(dests, destfd,
					      destoffsets, 1);
		}

		if (rv < 0) {
			if (rv == -1)
				done = 1;
			break;
		}

		while (rv) {
			unsigned long long offset;
			unsigned long actual_stripes;
			/* Need to backup some data.
			 * If 'part' is not used and the desired
			 * backup size is suspended, do a backup,
			 * then consider the next part.
			 */
			/* Check that 'part' is unused */
			if (part == 0 && __le64_to_cpu(bsb.length) != 0)
				break;
			if (part == 1 && __le64_to_cpu(bsb.length2) != 0)
				break;

			offset = backup_point / data;
			actual_stripes = stripes;
			if (increasing) {
				if (offset + actual_stripes * (chunk/512) >
				    sra->component_size)
					actual_stripes = ((sra->component_size - offset)
							  / (chunk/512));
				if (offset + actual_stripes * (chunk/512) >
				    suspend_point/data)
					break;
			} else {
				if (offset < actual_stripes * (chunk/512))
					actual_stripes = offset / (chunk/512);
				offset -= actual_stripes * (chunk/512);
				if (offset < suspend_point/data)
					break;
			}
			grow_backup(sra, offset, actual_stripes,
				    fds, offsets,
				    disks, chunk, level, layout,
				    dests, destfd, destoffsets,
				    part, &degraded, buf);
			validate(afd, destfd[0], destoffsets[0]);
			/* record where 'part' is up to */
			part = !part;
			if (increasing)
				backup_point += actual_stripes * (chunk/512) * data;
			else
				backup_point -= actual_stripes * (chunk/512) * data;
		}
	}

	/* FIXME maybe call progress_reshape one more time instead */
	abort_reshape(sra); /* remove any remaining suspension */
	if (reshape->before.data_disks == reshape->after.data_disks)
		sysfs_set_num(sra, NULL, "sync_speed_min", speed);
	free(buf);
	return done;
}

/*
 * If any spare contains md_back_data-1 which is recent wrt mtime,
 * write that data into the array and update the super blocks with
 * the new reshape_progress
 */
int Grow_restart(struct supertype *st, struct mdinfo *info, int *fdlist, int cnt,
		 char *backup_file, int verbose)
{
	int i, j;
	int old_disks;
	unsigned long long *offsets;
	unsigned long long  nstripe, ostripe;
	int ndata, odata;

	if (info->new_level != info->array.level)
		return 1; /* Cannot handle level changes (they are instantaneous) */

	odata = info->array.raid_disks - info->delta_disks - 1;
	if (info->array.level == 6) odata--; /* number of data disks */
	ndata = info->array.raid_disks - 1;
	if (info->new_level == 6) ndata--;

	old_disks = info->array.raid_disks - info->delta_disks;

	if (info->delta_disks <= 0)
		/* Didn't grow, so the backup file must have
		 * been used
		 */
		old_disks = cnt;
	for (i=old_disks-(backup_file?1:0); i<cnt; i++) {
		struct mdinfo dinfo;
		int fd;
		int bsbsize;
		char *devname, namebuf[20];
		unsigned long long lo, hi;

		/* This was a spare and may have some saved data on it.
		 * Load the superblock, find and load the
		 * backup_super_block.
		 * If either fail, go on to next device.
		 * If the backup contains no new info, just return
		 * else restore data and update all superblocks
		 */
		if (i == old_disks-1) {
			fd = open(backup_file, O_RDONLY);
			if (fd<0) {
				fprintf(stderr, Name ": backup file %s inaccessible: %s\n",
					backup_file, strerror(errno));
				continue;
			}
			devname = backup_file;
		} else {
			fd = fdlist[i];
			if (fd < 0)
				continue;
			if (st->ss->load_super(st, fd, NULL))
				continue;

			st->ss->getinfo_super(st, &dinfo, NULL);
			st->ss->free_super(st);

			if (lseek64(fd,
				    (dinfo.data_offset + dinfo.component_size - 8) <<9,
				    0) < 0) {
				fprintf(stderr, Name ": Cannot seek on device %d\n", i);
				continue; /* Cannot seek */
			}
			sprintf(namebuf, "device-%d", i);
			devname = namebuf;
		}
		if (read(fd, &bsb, sizeof(bsb)) != sizeof(bsb)) {
			if (verbose)
				fprintf(stderr, Name ": Cannot read from %s\n", devname);
			continue; /* Cannot read */
		}
		if (memcmp(bsb.magic, "md_backup_data-1", 16) != 0 &&
		    memcmp(bsb.magic, "md_backup_data-2", 16) != 0) {
			if (verbose)
				fprintf(stderr, Name ": No backup metadata on %s\n", devname);
			continue;
		}
		if (bsb.sb_csum != bsb_csum((char*)&bsb, ((char*)&bsb.sb_csum)-((char*)&bsb))) {
			if (verbose)
				fprintf(stderr, Name ": Bad backup-metadata checksum on %s\n", devname);
			continue; /* bad checksum */
		}
		if (memcmp(bsb.magic, "md_backup_data-2", 16) == 0 &&
		    bsb.sb_csum2 != bsb_csum((char*)&bsb, ((char*)&bsb.sb_csum2)-((char*)&bsb))) {
			if (verbose)
				fprintf(stderr, Name ": Bad backup-metadata checksum2 on %s\n", devname);
			continue; /* Bad second checksum */
		}
		if (memcmp(bsb.set_uuid,info->uuid, 16) != 0) {
			if (verbose)
				fprintf(stderr, Name ": Wrong uuid on backup-metadata on %s\n", devname);
			continue; /* Wrong uuid */
		}

		/* array utime and backup-mtime should be updated at much the same time, but it seems that
		 * sometimes they aren't... So allow considerable flexability in matching, and allow
		 * this test to be overridden by an environment variable.
		 */
		if (info->array.utime > (int)__le64_to_cpu(bsb.mtime) + 2*60*60 ||
		    info->array.utime < (int)__le64_to_cpu(bsb.mtime) - 10*60) {
			if (check_env("MDADM_GROW_ALLOW_OLD")) {
				fprintf(stderr, Name ": accepting backup with timestamp %lu "
					"for array with timestamp %lu\n",
					(unsigned long)__le64_to_cpu(bsb.mtime),
					(unsigned long)info->array.utime);
			} else {
				if (verbose)
					fprintf(stderr, Name ": too-old timestamp on "
						"backup-metadata on %s\n", devname);
				continue; /* time stamp is too bad */
			}
		}

		if (bsb.magic[15] == '1') {
			if (bsb.length == 0)
				continue;
			if (info->delta_disks >= 0) {
				/* reshape_progress is increasing */
				if (__le64_to_cpu(bsb.arraystart)
				    + __le64_to_cpu(bsb.length)
				    < info->reshape_progress) {
				nonew:
					if (verbose)
						fprintf(stderr, Name
                  ": backup-metadata found on %s but is not needed\n", devname);
					continue; /* No new data here */
				}
			} else {
				/* reshape_progress is decreasing */
				if (__le64_to_cpu(bsb.arraystart) >=
				    info->reshape_progress)
					goto nonew; /* No new data here */
			}
		} else {
			if (bsb.length == 0 && bsb.length2 == 0)
				continue;
			if (info->delta_disks >= 0) {
				/* reshape_progress is increasing */
				if ((__le64_to_cpu(bsb.arraystart)
				     + __le64_to_cpu(bsb.length)
				     < info->reshape_progress)
				    &&
				    (__le64_to_cpu(bsb.arraystart2)
				     + __le64_to_cpu(bsb.length2)
				     < info->reshape_progress))
					goto nonew; /* No new data here */
			} else {
				/* reshape_progress is decreasing */
				if (__le64_to_cpu(bsb.arraystart) >=
				    info->reshape_progress &&
				    __le64_to_cpu(bsb.arraystart2) >=
				    info->reshape_progress)
					goto nonew; /* No new data here */
			}
		}
		if (lseek64(fd, __le64_to_cpu(bsb.devstart)*512, 0)< 0) {
		second_fail:
			if (verbose)
				fprintf(stderr, Name
		     ": Failed to verify secondary backup-metadata block on %s\n",
					devname);
			continue; /* Cannot seek */
		}
		/* There should be a duplicate backup superblock 4k before here */
		if (lseek64(fd, -4096, 1) < 0 ||
		    read(fd, &bsb2, sizeof(bsb2)) != sizeof(bsb2))
			goto second_fail; /* Cannot find leading superblock */
		if (bsb.magic[15] == '1')
			bsbsize = offsetof(struct mdp_backup_super, pad1);
		else
			bsbsize = offsetof(struct mdp_backup_super, pad);
		if (memcmp(&bsb2, &bsb, bsbsize) != 0)
			goto second_fail; /* Cannot find leading superblock */

		/* Now need the data offsets for all devices. */
		offsets = malloc(sizeof(*offsets)*info->array.raid_disks);
		for(j=0; j<info->array.raid_disks; j++) {
			if (fdlist[j] < 0)
				continue;
			if (st->ss->load_super(st, fdlist[j], NULL))
				/* FIXME should be this be an error */
				continue;
			st->ss->getinfo_super(st, &dinfo, NULL);
			st->ss->free_super(st);
			offsets[j] = dinfo.data_offset * 512;
		}
		printf(Name ": restoring critical section\n");

		if (restore_stripes(fdlist, offsets,
				    info->array.raid_disks,
				    info->new_chunk,
				    info->new_level,
				    info->new_layout,
				    fd, __le64_to_cpu(bsb.devstart)*512,
				    __le64_to_cpu(bsb.arraystart)*512,
				    __le64_to_cpu(bsb.length)*512)) {
			/* didn't succeed, so giveup */
			if (verbose)
				fprintf(stderr, Name ": Error restoring backup from %s\n",
					devname);
			return 1;
		}
		
		if (bsb.magic[15] == '2' &&
		    restore_stripes(fdlist, offsets,
				    info->array.raid_disks,
				    info->new_chunk,
				    info->new_level,
				    info->new_layout,
				    fd, __le64_to_cpu(bsb.devstart)*512 +
				    __le64_to_cpu(bsb.devstart2)*512,
				    __le64_to_cpu(bsb.arraystart2)*512,
				    __le64_to_cpu(bsb.length2)*512)) {
			/* didn't succeed, so giveup */
			if (verbose)
				fprintf(stderr, Name ": Error restoring second backup from %s\n",
					devname);
			return 1;
		}


		/* Ok, so the data is restored. Let's update those superblocks. */

		lo = hi = 0;
		if (bsb.length) {
			lo = __le64_to_cpu(bsb.arraystart);
			hi = lo + __le64_to_cpu(bsb.length);
		}
		if (bsb.magic[15] == '2' && bsb.length2) {
			unsigned long long lo1, hi1;
			lo1 = __le64_to_cpu(bsb.arraystart2);
			hi1 = lo1 + __le64_to_cpu(bsb.length2);
			if (lo == hi) {
				lo = lo1;
				hi = hi1;
			} else if (lo < lo1)
				hi = hi1;
			else
				lo = lo1;
		}
		if (lo < hi &&
		    (info->reshape_progress < lo ||
		     info->reshape_progress > hi))
			/* backup does not affect reshape_progress*/ ;
		else if (info->delta_disks >= 0) {
			info->reshape_progress = __le64_to_cpu(bsb.arraystart) +
				__le64_to_cpu(bsb.length);
			if (bsb.magic[15] == '2') {
				unsigned long long p2 = __le64_to_cpu(bsb.arraystart2) +
					__le64_to_cpu(bsb.length2);
				if (p2 > info->reshape_progress)
					info->reshape_progress = p2;
			}
		} else {
			info->reshape_progress = __le64_to_cpu(bsb.arraystart);
			if (bsb.magic[15] == '2') {
				unsigned long long p2 = __le64_to_cpu(bsb.arraystart2);
				if (p2 < info->reshape_progress)
					info->reshape_progress = p2;
			}
		}
		for (j=0; j<info->array.raid_disks; j++) {
			if (fdlist[j] < 0) continue;
			if (st->ss->load_super(st, fdlist[j], NULL))
				continue;
			st->ss->getinfo_super(st, &dinfo, NULL);
			dinfo.reshape_progress = info->reshape_progress;
			st->ss->update_super(st, &dinfo,
					     "_reshape_progress",
					     NULL,0, 0, NULL);
			st->ss->store_super(st, fdlist[j]);
			st->ss->free_super(st);
		}
		return 0;
	}
	/* Didn't find any backup data, try to see if any
	 * was needed.
	 */
	if (info->delta_disks < 0) {
		/* When shrinking, the critical section is at the end.
		 * So see if we are before the critical section.
		 */
		unsigned long long first_block;
		nstripe = ostripe = 0;
		first_block = 0;
		while (ostripe >= nstripe) {
			ostripe += info->array.chunk_size / 512;
			first_block = ostripe * odata;
			nstripe = first_block / ndata / (info->new_chunk/512) *
				(info->new_chunk/512);
		}

		if (info->reshape_progress >= first_block)
			return 0;
	}
	if (info->delta_disks > 0) {
		/* See if we are beyond the critical section. */
		unsigned long long last_block;
		nstripe = ostripe = 0;
		last_block = 0;
		while (nstripe >= ostripe) {
			nstripe += info->new_chunk / 512;
			last_block = nstripe * ndata;
			ostripe = last_block / odata / (info->array.chunk_size/512) *
				(info->array.chunk_size/512);
		}

		if (info->reshape_progress >= last_block)
			return 0;
	}
	/* needed to recover critical section! */
	if (verbose)
		fprintf(stderr, Name ": Failed to find backup of critical section\n");
	return 1;
}

int Grow_continue(int mdfd, struct supertype *st, struct mdinfo *info,
		  char *backup_file)
{
	int err = sysfs_set_str(info, NULL, "array_state", "readonly");
	if (err)
		return err;
	return reshape_array(NULL, mdfd, "array", st, info, 1, backup_file, 0, 0, 1);
}


