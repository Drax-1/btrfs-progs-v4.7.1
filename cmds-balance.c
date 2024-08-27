/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

static int cmd_balance_pause(int argc, char **argv);
static int cmd_balance_cancel(int argc, char **argv);
static int cmd_balance_resume(int argc, char **argv);
static int cmd_balance_status(int argc, char **argv);

static const char * const cmd_balance_pause_usage[];
static const char * const cmd_balance_cancel_usage[];
static const char * const cmd_balance_resume_usage[];
static const char * const cmd_balance_status_usage[];

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "kerncompat.h"
#include "ctree.h"
#include "ioctl.h"
#include "volumes.h"

#include "commands.h"
#include "utils.h"

static const char * const balance_cmd_group_usage[] = {
	"btrfs balance <command> [options] <path>",
	"btrfs balance <path>",
	NULL
};

static int parse_one_profile(const char *profile, u64 *flags)
{
	if (!strcmp(profile, "raid0")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID0;
	} else if (!strcmp(profile, "raid1")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID1;
	} else if (!strcmp(profile, "raid10")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID10;
	} else if (!strcmp(profile, "raid5")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID5;
	} else if (!strcmp(profile, "raid6")) {
		*flags |= BTRFS_BLOCK_GROUP_RAID6;
	} else if (!strcmp(profile, "dup")) {
		*flags |= BTRFS_BLOCK_GROUP_DUP;
	} else if (!strcmp(profile, "single")) {
		*flags |= BTRFS_AVAIL_ALLOC_BIT_SINGLE;
	} else {
		error("unknown profile: %s", profile);
		return 1;
	}

	return 0;
}

static int parse_profiles(char *profiles, u64 *flags)
{
	char *this_char;
	char *save_ptr = NULL; /* Satisfy static checkers */

	for (this_char = strtok_r(profiles, "|", &save_ptr);
	     this_char != NULL;
	     this_char = strtok_r(NULL, "|", &save_ptr)) {
		if (parse_one_profile(this_char, flags))
			return 1;
	}

	return 0;
}

static int parse_u64(const char *str, u64 *result)
{
	char *endptr;
	u64 val;

	val = strtoull(str, &endptr, 10);
	if (*endptr)
		return 1;

	*result = val;
	return 0;
}

/*
 * Parse range that's missing some part that can be implicit:
 * a..b	- exact range, a can be equal to b
 * a..	- implicitly unbounded maximum (end == (u64)-1)
 * ..b	- implicitly starting at 0
 * a	- invalid; unclear semantics, use parse_u64 instead
 *
 * Returned values are u64, value validation and interpretation should be done
 * by the caller.
 */
static int parse_range(const char *range, u64 *start, u64 *end)
{
	char *dots;
	char *endptr;
	const char *rest;
	int skipped = 0;

	dots = strstr(range, "..");
	if (!dots)
		return 1;

	rest = dots + 2;

	if (!*rest) {
		*end = (u64)-1;
		skipped++;
	} else {
		*end = strtoull(rest, &endptr, 10);
		if (*endptr)
			return 1;
	}
	if (dots == range) {
		*start = 0;
		skipped++;
	} else {
		*start = strtoull(range, &endptr, 10);
		if (*endptr != 0 && *endptr != '.')
			return 1;
	}

	if (*start > *end) {
		error("range %llu..%llu doesn't make sense",
			(unsigned long long)*start,
			(unsigned long long)*end);
		return 1;
	}

	if (skipped <= 1)
		return 0;

	return 1;
}

/*
 * Parse range and check if start < end
 */
static int parse_range_strict(const char *range, u64 *start, u64 *end)
{
	if (parse_range(range, start, end) == 0) {
		if (*start >= *end) {
			error("range %llu..%llu not allowed",
				(unsigned long long)*start,
				(unsigned long long)*end);
			return 1;
		}
		return 0;
	}

	return 1;
}

/*
 * Convert 64bit range to 32bit with boundary checks
 */
static int range_to_u32(u64 start, u64 end, u32 *start32, u32 *end32)
{
	if (start > (u32)-1)
		return 1;

	if (end != (u64)-1 && end > (u32)-1)
		return 1;

	*start32 = (u32)start;
	*end32 = (u32)end;

	return 0;
}

__attribute__ ((unused))
static int parse_range_u32(const char *range, u32 *start, u32 *end)
{
	u64 tmp_start;
	u64 tmp_end;

	if (parse_range(range, &tmp_start, &tmp_end))
		return 1;

	if (range_to_u32(tmp_start, tmp_end, start, end))
		return 1;

	return 0;
}

__attribute__ ((unused))
static void print_range(u64 start, u64 end)
{
	if (start)
		printf("%llu", (unsigned long long)start);
	printf("..");
	if (end != (u64)-1)
		printf("%llu", (unsigned long long)end);
}

__attribute__ ((unused))
static void print_range_u32(u32 start, u32 end)
{
	if (start)
		printf("%u", start);
	printf("..");
	if (end != (u32)-1)
		printf("%u", end);
}

int parse_filters(char *filters, struct btrfs_balance_args *args)
{
    char *this_char;
    char *value;
    char *save_ptr = NULL; /* Satisfy static checkers */

    if (!filters)
        return 0;

    for (this_char = strtok_r(filters, ",", &save_ptr);
         this_char != NULL;
         this_char = strtok_r(NULL, ",", &save_ptr)) {
        if ((value = strchr(this_char, '=')) != NULL)
            *value++ = 0;

        if (!strcmp(this_char, "profiles")) {
            /* ... */
            __u64 tmp_profiles;
            memcpy(&tmp_profiles, &args->profiles, sizeof(tmp_profiles));
            if (parse_profiles(value, &tmp_profiles)) {
                error("invalid profiles argument");
                return 1;
            }
            memcpy(&args->profiles, &tmp_profiles, sizeof(args->profiles));
            /* ... */
        } else if (!strcmp(this_char, "usage")) {
            /* ... */
            __u64 tmp_usage;
            __u32 tmp_usage_min, tmp_usage_max;
            memcpy(&tmp_usage, &args->usage, sizeof(tmp_usage));
            memcpy(&tmp_usage_min, &args->usage_min, sizeof(tmp_usage_min));
            memcpy(&tmp_usage_max, &args->usage_max, sizeof(tmp_usage_max));
            if (parse_u64(value, &tmp_usage)) {
                if (parse_range_u32(value, &tmp_usage_min, &tmp_usage_max)) {
                    error("invalid usage argument: %s", value);
                    return 1;
                }
                memcpy(&args->usage_min, &tmp_usage_min, sizeof(args->usage_min));
                memcpy(&args->usage_max, &tmp_usage_max, sizeof(args->usage_max));
                args->flags &= ~BTRFS_BALANCE_ARGS_USAGE;
                args->flags |= BTRFS_BALANCE_ARGS_USAGE_RANGE;
            } else {
                memcpy(&args->usage, &tmp_usage, sizeof(args->usage));
                args->flags &= ~BTRFS_BALANCE_ARGS_USAGE_RANGE;
                args->flags |= BTRFS_BALANCE_ARGS_USAGE;
            }
            /* ... */
        } else if (!strcmp(this_char, "devid")) {
            /* ... */
            __u64 tmp_devid;
            memcpy(&tmp_devid, &args->devid, sizeof(tmp_devid));
            if (parse_u64(value, &tmp_devid) || tmp_devid == 0) {
                error("invalid devid argument: %s", value);
                return 1;
            }
            memcpy(&args->devid, &tmp_devid, sizeof(args->devid));
            /* ... */
        } else if (!strcmp(this_char, "drange")) {
            /* ... */
            __u64 tmp_pstart, tmp_pend;
            memcpy(&tmp_pstart, &args->pstart, sizeof(tmp_pstart));
            memcpy(&tmp_pend, &args->pend, sizeof(tmp_pend));
            if (parse_range_strict(value, &tmp_pstart, &tmp_pend)) {
                error("invalid drange argument");
                return 1;
            }
            memcpy(&args->pstart, &tmp_pstart, sizeof(args->pstart));
            memcpy(&args->pend, &tmp_pend, sizeof(args->pend));
            /* ... */
        } else if (!strcmp(this_char, "vrange")) {
            /* ... */
            __u64 tmp_vstart, tmp_vend;
            memcpy(&tmp_vstart, &args->vstart, sizeof(tmp_vstart));
            memcpy(&tmp_vend, &args->vend, sizeof(tmp_vend));
            if (parse_range_strict(value, &tmp_vstart, &tmp_vend)) {
                error("invalid vrange argument");
                return 1;
            }
            memcpy(&args->vstart, &tmp_vstart, sizeof(args->vstart));
            memcpy(&args->vend, &tmp_vend, sizeof(args->vend));
            /* ... */
        } else if (!strcmp(this_char, "convert")) {
            /* ... */
            __u64 tmp_target;
            memcpy(&tmp_target, &args->target, sizeof(tmp_target));
            if (parse_one_profile(value, &tmp_target)) {
                error("invalid convert argument");
                return 1;
            }
            memcpy(&args->target, &tmp_target, sizeof(args->target));
            /* ... */
        } else if (!strcmp(this_char, "limit")) {
            /* ... */
            __u64 tmp_limit;
            __u32 tmp_limit_min, tmp_limit_max;
            memcpy(&tmp_limit, &args->limit, sizeof(tmp_limit));
            memcpy(&tmp_limit_min, &args->limit_min, sizeof(tmp_limit_min));
            memcpy(&tmp_limit_max, &args->limit_max, sizeof(tmp_limit_max));
            if (parse_u64(value, &tmp_limit)) {
                if (parse_range_u32(value, &tmp_limit_min, &tmp_limit_max)) {
                    error("Invalid limit argument: %s", value);
                    return 1;
                }
                memcpy(&args->limit_min, &tmp_limit_min, sizeof(args->limit_min));
                memcpy(&args->limit_max, &tmp_limit_max, sizeof(args->limit_max));
                args->flags &= ~BTRFS_BALANCE_ARGS_LIMIT;
                args->flags |= BTRFS_BALANCE_ARGS_LIMIT_RANGE;
            } else {
                memcpy(&args->limit, &tmp_limit, sizeof(args->limit));
                args->flags &= ~BTRFS_BALANCE_ARGS_LIMIT_RANGE;
                args->flags |= BTRFS_BALANCE_ARGS_LIMIT;
            }
            /* ... */
        } else if (!strcmp(this_char, "stripes")) {
            /* ... */
            __u32 tmp_stripes_min, tmp_stripes_max;
            memcpy(&tmp_stripes_min, &args->stripes_min, sizeof(tmp_stripes_min));
            memcpy(&tmp_stripes_max, &args->stripes_max, sizeof(tmp_stripes_max));
            if (parse_range_u32(value, &tmp_stripes_min, &tmp_stripes_max)) {
                error("invalid stripes argument");
                return 1;
            }
            memcpy(&args->stripes_min, &tmp_stripes_min, sizeof(args->stripes_min));
            memcpy(&args->stripes_max, &tmp_stripes_max, sizeof(args->stripes_max));
            /* ... */
        } else {
            error("unrecognized balance option: %s", this_char);
            return 1;
        }
    }

    return 0;
}

static void dump_balance_args(struct btrfs_balance_args *args)
{
	if (args->flags & BTRFS_BALANCE_ARGS_CONVERT) {
		printf("converting, target=%llu, soft is %s",
		       (unsigned long long)args->target,
		       (args->flags & BTRFS_BALANCE_ARGS_SOFT) ? "on" : "off");
	} else {
		printf("balancing");
	}

	if (args->flags & BTRFS_BALANCE_ARGS_PROFILES)
		printf(", profiles=%llu", (unsigned long long)args->profiles);
	if (args->flags & BTRFS_BALANCE_ARGS_USAGE)
		printf(", usage=%llu", (unsigned long long)args->usage);
	if (args->flags & BTRFS_BALANCE_ARGS_USAGE_RANGE) {
		printf(", usage=");
		print_range_u32(args->usage_min, args->usage_max);
	}
	if (args->flags & BTRFS_BALANCE_ARGS_DEVID)
		printf(", devid=%llu", (unsigned long long)args->devid);
	if (args->flags & BTRFS_BALANCE_ARGS_DRANGE)
		printf(", drange=%llu..%llu",
		       (unsigned long long)args->pstart,
		       (unsigned long long)args->pend);
	if (args->flags & BTRFS_BALANCE_ARGS_VRANGE)
		printf(", vrange=%llu..%llu",
		       (unsigned long long)args->vstart,
		       (unsigned long long)args->vend);
	if (args->flags & BTRFS_BALANCE_ARGS_LIMIT)
		printf(", limit=%llu", (unsigned long long)args->limit);
	if (args->flags & BTRFS_BALANCE_ARGS_LIMIT_RANGE) {
		printf(", limit=");
		print_range_u32(args->limit_min, args->limit_max);
	}
	if (args->flags & BTRFS_BALANCE_ARGS_STRIPES_RANGE) {
		printf(", stripes=");
		print_range_u32(args->stripes_min, args->stripes_max);
	}

	printf("\n");
}

static void dump_ioctl_balance_args(struct btrfs_ioctl_balance_args *args)
{
	printf("Dumping filters: flags 0x%llx, state 0x%llx, force is %s\n",
	       (unsigned long long)args->flags, (unsigned long long)args->state,
	       (args->flags & BTRFS_BALANCE_FORCE) ? "on" : "off");
	if (args->flags & BTRFS_BALANCE_DATA) {
		printf("  DATA (flags 0x%llx): ",
		       (unsigned long long)args->data.flags);
		dump_balance_args(&args->data);
	}
	if (args->flags & BTRFS_BALANCE_METADATA) {
		printf("  METADATA (flags 0x%llx): ",
		       (unsigned long long)args->meta.flags);
		dump_balance_args(&args->meta);
	}
	if (args->flags & BTRFS_BALANCE_SYSTEM) {
		printf("  SYSTEM (flags 0x%llx): ",
		       (unsigned long long)args->sys.flags);
		dump_balance_args(&args->sys);
	}
}

static int do_balance_v1(int fd)
{
	struct btrfs_ioctl_vol_args args;
	int ret;

	memset(&args, 0, sizeof(args));
	ret = ioctl(fd, BTRFS_IOC_BALANCE, &args);
	return ret;
}

enum {
	BALANCE_START_FILTERS = 1 << 0,
	BALANCE_START_NOWARN  = 1 << 1
};

static int do_balance(const char *path, struct btrfs_ioctl_balance_args *args,
		      unsigned flags)
{
	int fd;
	int ret;
	DIR *dirstream = NULL;

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	if (!(flags & BALANCE_START_FILTERS) && !(flags & BALANCE_START_NOWARN)) {
		int delay = 10;

		printf("WARNING:\n\n");
		printf("\tFull balance without filters requested. This operation is very\n");
		printf("\tintense and takes potentially very long. It is recommended to\n");
		printf("\tuse the balance filters to narrow down the balanced data.\n");
		printf("\tUse 'btrfs balance start --full-balance' option to skip this\n");
		printf("\twarning. The operation will start in %d seconds.\n", delay);
		printf("\tUse Ctrl-C to stop it.\n");
		while (delay) {
			printf("%2d", delay--);
			fflush(stdout);
			sleep(1);
		}
		printf("\nStarting balance without any filters.\n");
	}

	ret = ioctl(fd, BTRFS_IOC_BALANCE_V2, args);
	if (ret < 0) {
		/*
		 * older kernels don't have the new balance ioctl, try the
		 * old one.  But, the old one doesn't know any filters, so
		 * don't fall back if they tried to use the fancy new things
		 */
		if (errno == ENOTTY && !(flags & BALANCE_START_FILTERS)) {
			ret = do_balance_v1(fd);
			if (ret == 0)
				goto out;
		}

		if (errno == ECANCELED) {
			if (args->state & BTRFS_BALANCE_STATE_PAUSE_REQ)
				fprintf(stderr, "balance paused by user\n");
			if (args->state & BTRFS_BALANCE_STATE_CANCEL_REQ)
				fprintf(stderr, "balance canceled by user\n");
			ret = 0;
		} else {
			error("error during balancing '%s': %s", path,
					strerror(errno));
			if (errno != EINPROGRESS)
				fprintf(stderr,
			"There may be more info in syslog - try dmesg | tail\n");
			ret = 1;
		}
	} else {
		printf("Done, had to relocate %llu out of %llu chunks\n",
		       (unsigned long long)args->stat.completed,
		       (unsigned long long)args->stat.considered);
		ret = 0;
	}

out:
	close_file_or_dir(fd, dirstream);
	return ret;
}

static const char * const cmd_balance_start_usage[] = {
	"btrfs balance start [options] <path>",
	"Balance chunks across the devices",
	"Balance and/or convert (change allocation profile of) chunks that",
	"passed all filters in a comma-separated list of filters for a",
	"particular chunk type.  If filter list is not given balance all",
	"chunks of that type.  In case none of the -d, -m or -s options is",
	"given balance all chunks in a filesystem. This is potentially",
	"long operation and the user is warned before this start, with",
	"a delay to stop it.",
	"",
	"-d[filters]    act on data chunks",
	"-m[filters]    act on metadata chunks",
	"-s[filters]    act on system chunks (only under -f)",
	"-v             be verbose",
	"-f             force reducing of metadata integrity",
	"--full-balance do not print warning and do not delay start",
	"--background|--bg",
	"               run the balance as a background process",
	NULL
};

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "ioctl.h"
#include "utils.h"
#include "commands.h"

static int cmd_balance_start(int argc, char **argv)
{
    struct btrfs_ioctl_balance_args args;
    int ret = 0;
    int force = 0;
    // Remove these lines
// int verbose = 0;
// int background = 0;
    u64 start_flags = 0;
    char *path;

    memset(&args, 0, sizeof(args));

    while (1) {
        int c = getopt(argc, argv, "dflmszLD:pvV");
        if (c < 0)
            break;

        switch (c) {
        case 'd':
            args.flags |= BTRFS_BALANCE_DATA;
            break;
        case 'f':
            force = 1;
            break;
        case 'l':
            start_flags |= BALANCE_START_FILTERS;
            break;
        // ... (other cases)
        default:
            usage(cmd_balance_start_usage);
        }
    }

    if (check_argc_exact(argc - optind, 1))
        return 1;

    path = argv[optind];

    if (args.flags & BTRFS_BALANCE_SYSTEM) {
        fprintf(stderr, "WARNING: system chunk balancing is not recommended\n");
        if (!force) {
            fprintf(stderr, "  Use --force if you want to continue\n");
            return 1;
        }
    } else if (args.flags & BTRFS_BALANCE_METADATA) {
        fprintf(stderr, "WARNING: metadata chunk balancing is not recommended\n");
        if (!force) {
            fprintf(stderr, "  Use --force if you want to continue\n");
            return 1;
        }
    }

    if (!(start_flags & BALANCE_START_FILTERS)) {
        fprintf(stderr, "No filters or profiles specified, use -d, -m or -s options\n"
                        "to specify filters or --full-balance to run a full balance\n");
        return 1;
    }

    ret = do_balance(path, &args, start_flags);

    return ret;
}


const struct cmd_group balance_cmd_group = {
    balance_cmd_group_usage, NULL, {
        { "start", cmd_balance_start, cmd_balance_start_usage, NULL, 0 },
        { "pause", cmd_balance_pause, cmd_balance_pause_usage, NULL, 0 },
        { "cancel", cmd_balance_cancel, cmd_balance_cancel_usage, NULL, 0 },
        { "resume", cmd_balance_resume, cmd_balance_resume_usage, NULL, 0 },
        { "status", cmd_balance_status, cmd_balance_status_usage, NULL, 0 },
        NULL_CMD_STRUCT
    }
};
		

static const char * const cmd_balance_pause_usage[] = {
	"btrfs balance pause <path>",
	"Pause running balance",
	NULL
};

static int cmd_balance_pause(int argc, char **argv)
{
	const char *path;
	int fd;
	int ret;
	DIR *dirstream = NULL;

	clean_args_no_options(argc, argv, cmd_balance_pause_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_balance_pause_usage);

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_CTL, BTRFS_BALANCE_CTL_PAUSE);
	if (ret < 0) {
		error("balance pause on '%s' failed: %s", path,
			(errno == ENOTCONN) ? "Not running" : strerror(errno));
		if (errno == ENOTCONN)
			ret = 2;
		else
			ret = 1;
	}

	close_file_or_dir(fd, dirstream);
	return ret;
}

static const char * const cmd_balance_cancel_usage[] = {
	"btrfs balance cancel <path>",
	"Cancel running or paused balance",
	NULL
};

static int cmd_balance_cancel(int argc, char **argv)
{
	const char *path;
	int fd;
	int ret;
	DIR *dirstream = NULL;

	clean_args_no_options(argc, argv, cmd_balance_cancel_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_balance_cancel_usage);

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_CTL, BTRFS_BALANCE_CTL_CANCEL);
	if (ret < 0) {
		error("balance cancel on '%s' failed: %s", path,
			(errno == ENOTCONN) ? "Not in progress" : strerror(errno));
		if (errno == ENOTCONN)
			ret = 2;
		else
			ret = 1;
	}

	close_file_or_dir(fd, dirstream);
	return ret;
}

static const char * const cmd_balance_resume_usage[] = {
	"btrfs balance resume <path>",
	"Resume interrupted balance",
	NULL
};

static int cmd_balance_resume(int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	const char *path;
	DIR *dirstream = NULL;
	int fd;
	int ret;

	clean_args_no_options(argc, argv, cmd_balance_resume_usage);

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_balance_resume_usage);

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 1;

	memset(&args, 0, sizeof(args));
	args.flags |= BTRFS_BALANCE_RESUME;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_V2, &args);
	if (ret < 0) {
		if (errno == ECANCELED) {
			if (args.state & BTRFS_BALANCE_STATE_PAUSE_REQ)
				fprintf(stderr, "balance paused by user\n");
			if (args.state & BTRFS_BALANCE_STATE_CANCEL_REQ)
				fprintf(stderr, "balance canceled by user\n");
		} else if (errno == ENOTCONN || errno == EINPROGRESS) {
			error("balance resume on '%s' failed: %s", path,
				(errno == ENOTCONN) ? "Not in progress" :
						  "Already running");
			if (errno == ENOTCONN)
				ret = 2;
			else
				ret = 1;
		} else {
			error("error during balancing '%s': %s\n"
			  "There may be more info in syslog - try dmesg | tail",
				path, strerror(errno));
			ret = 1;
		}
	} else {
		printf("Done, had to relocate %llu out of %llu chunks\n",
		       (unsigned long long)args.stat.completed,
		       (unsigned long long)args.stat.considered);
	}

	close_file_or_dir(fd, dirstream);
	return ret;
}

static const char * const cmd_balance_status_usage[] = {
	"btrfs balance status [-v] <path>",
	"Show status of running or paused balance",
	"",
	"-v     be verbose",
	NULL
};

/* Checks the status of the balance if any
 * return codes:
 *   2 : Error failed to know if there is any pending balance
 *   1 : Successful to know status of a pending balance
 *   0 : When there is no pending balance or completed
 */
static int cmd_balance_status(int argc, char **argv)
{
	struct btrfs_ioctl_balance_args args;
	const char *path;
	DIR *dirstream = NULL;
	int fd;
	int verbose = 0;
	int ret;

	while (1) {
		int opt;
		static const struct option longopts[] = {
			{ "verbose", no_argument, NULL, 'v' },
			{ NULL, 0, NULL, 0 }
		};

		opt = getopt_long(argc, argv, "v", longopts, NULL);
		if (opt < 0)
			break;

		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		default:
			usage(cmd_balance_status_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_balance_status_usage);

	path = argv[optind];

	fd = btrfs_open_dir(path, &dirstream, 1);
	if (fd < 0)
		return 2;

	ret = ioctl(fd, BTRFS_IOC_BALANCE_PROGRESS, &args);
	if (ret < 0) {
		if (errno == ENOTCONN) {
			printf("No balance found on '%s'\n", path);
			ret = 0;
			goto out;
		}
		error("balance status on '%s' failed: %s", path, strerror(errno));
		ret = 2;
		goto out;
	}

	if (args.state & BTRFS_BALANCE_STATE_RUNNING) {
		printf("Balance on '%s' is running", path);
		if (args.state & BTRFS_BALANCE_STATE_CANCEL_REQ)
			printf(", cancel requested\n");
		else if (args.state & BTRFS_BALANCE_STATE_PAUSE_REQ)
			printf(", pause requested\n");
		else
			printf("\n");
	} else {
		printf("Balance on '%s' is paused\n", path);
	}

	printf("%llu out of about %llu chunks balanced (%llu considered), "
	       "%3.f%% left\n", (unsigned long long)args.stat.completed,
	       (unsigned long long)args.stat.expected,
	       (unsigned long long)args.stat.considered,
	       100 * (1 - (float)args.stat.completed/args.stat.expected));

	if (verbose)
		dump_ioctl_balance_args(&args);

	ret = 1;
out:
	close_file_or_dir(fd, dirstream);
	return ret;
}





int cmd_balance(int argc, char **argv)
{
	if (argc == 2 && strcmp("start", argv[1]) != 0) {
		/* old 'btrfs filesystem balance <path>' syntax */
		struct btrfs_ioctl_balance_args args;

		memset(&args, 0, sizeof(args));
		args.flags |= BTRFS_BALANCE_TYPE_MASK;

		return do_balance(argv[1], &args, 0);
	}

	return handle_command_group(&balance_cmd_group, argc, argv);
}
