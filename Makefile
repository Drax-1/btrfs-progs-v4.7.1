# btrfs-progs
#
# Basic build targets:
#   all		all main tools
#   static      build static bnaries, requires static version of the libraries
#   test        run the full testsuite
#   install     install to default location (/usr/local)
#   clean       clean built binaries (not the documentation)
#   clean-all   clean as above, clean docs and generated files
#
# Tuning by variables (environment or make arguments):
#   V=1            verbose, print command lines (default: quiet)
#   C=1            run checker before compilation (default checker: sparse)
#   D=1            debugging build, turn off optimizations
#   D=dflags       dtto, turn on additional debugging features:
#                  verbose - print file:line along with error/warning messages
#                  trace   - print trace before the error/warning messages
#                  abort   - call abort() on first error (dumps core)
#                  all     - shortcut for all of the above
#   W=123          build with warnings (default: off)
#   DEBUG_CFLAGS   additional compiler flags for debugging build
#   EXTRA_CFLAGS   additional compiler flags
#   EXTRA_LDFLAGS  additional linker flags
#
# Static checkers:
#   CHECKER        static checker binary to be called (default: sparse)
#   CHECKER_FLAGS  flags to pass to CHECKER, can override CFLAGS
#

# Export all variables to sub-makes by default
export

include Makefile.extrawarn

CC = gcc
LN_S = ln -s
AR = ar
RM = /usr/bin/rm
RMDIR = /usr/bin/rmdir
INSTALL = /usr/bin/install -c
DISABLE_DOCUMENTATION = 0
DISABLE_BTRFSCONVERT = 0
BTRFSCONVERT_EXT2 = 1

EXTRA_CFLAGS :=
EXTRA_LDFLAGS :=

DEBUG_CFLAGS_DEFAULT = -O0 -U_FORTIFY_SOURCE -ggdb3
DEBUG_CFLAGS_INTERNAL =
DEBUG_CFLAGS :=

# Common build flags
CFLAGS = -g -O1 -Wall -D_FORTIFY_SOURCE=2 \
	 -include config.h \
	 -DBTRFS_FLAT_INCLUDES \
	 -D_XOPEN_SOURCE=700  \
	 -fno-strict-aliasing \
	 -fPIC \
	 $(EXTRAWARN_CFLAGS) \
	 $(DEBUG_CFLAGS_INTERNAL) \
	 $(EXTRA_CFLAGS)

LDFLAGS =  \
	  -rdynamic $(EXTRA_LDFLAGS)

LIBS = -luuid  -lblkid  -lz  -llzo2 -L. -pthread
LIBBTRFS_LIBS = $(LIBS)

# Static compilation flags
STATIC_CFLAGS = $(CFLAGS) -ffunction-sections -fdata-sections
STATIC_LDFLAGS = -static -Wl,--gc-sections
STATIC_LIBS = -luuid  -lblkid  \
	      -lz  -llzo2 -L. -pthread

# don't use FORTIFY with sparse because glibc with FORTIFY can
# generate so many sparse errors that sparse stops parsing,
# which masks real errors that we want to see.
CHECKER := sparse
check_defs := .cc-defines.h
CHECKER_FLAGS := -include $(check_defs) -D__CHECKER__ \
	-D__CHECK_ENDIAN__ -Wbitwise -Wuninitialized -Wshadow -Wundef \
	-U_FORTIFY_SOURCE

objects = ctree.o disk-io.o radix-tree.o extent-tree.o print-tree.o \
	  root-tree.o dir-item.o file-item.o inode-item.o inode-map.o \
	  extent-cache.o extent_io.o volumes.o utils.o repair.o \
	  qgroup.o raid6.o free-space-cache.o list_sort.o props.o \
	  ulist.o qgroup-verify.o backref.o string-table.o task-utils.o \
	  inode.o file.o find-root.o free-space-tree.o help.o
cmds_objects = cmds-subvolume.o cmds-filesystem.o cmds-device.o cmds-scrub.o \
	       cmds-inspect.o cmds-balance.o cmds-send.o cmds-receive.o \
	       cmds-quota.o cmds-qgroup.o cmds-replace.o cmds-check.o \
	       cmds-restore.o cmds-rescue.o chunk-recover.o super-recover.o \
	       cmds-property.o cmds-fi-usage.o cmds-inspect-dump-tree.o \
	       cmds-inspect-dump-super.o cmds-inspect-tree-stats.o cmds-fi-du.o
libbtrfs_objects = send-stream.o send-utils.o rbtree.o btrfs-list.o crc32c.o \
		   uuid-tree.o utils-lib.o rbtree-utils.o
libbtrfs_headers = send-stream.h send-utils.h send.h rbtree.h btrfs-list.h \
	       crc32c.h list.h kerncompat.h radix-tree.h extent-cache.h \
	       extent_io.h ioctl.h ctree.h btrfsck.h version.h
TESTS = fsck-tests.sh convert-tests.sh

udev_rules = 64-btrfs-dm.rules

prefix ?= /usr/local
exec_prefix = ${prefix}
bindir = ${exec_prefix}/bin
libdir ?= ${exec_prefix}/lib
incdir = ${prefix}/include/btrfs
udevdir = /usr/lib/udev
udevruledir = ${udevdir}/rules.d

ifeq ("$(origin V)", "command line")
  BUILD_VERBOSE = $(V)
endif
ifndef BUILD_VERBOSE
  BUILD_VERBOSE = 0
endif

ifeq ($(BUILD_VERBOSE),1)
  Q =
else
  Q = @
endif

ifeq ("$(origin D)", "command line")
  DEBUG_CFLAGS_INTERNAL = $(DEBUG_CFLAGS_DEFAULT) $(DEBUG_CFLAGS)
endif

ifneq (,$(findstring verbose,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_VERBOSE_ERROR=1
endif

ifneq (,$(findstring trace,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_TRACE_ON_ERROR=1
endif

ifneq (,$(findstring abort,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_ABORT_ON_ERROR=1
endif

ifneq (,$(findstring all,$(D)))
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_VERBOSE_ERROR=1
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_TRACE_ON_ERROR=1
  DEBUG_CFLAGS_INTERNAL += -DDEBUG_ABORT_ON_ERROR=1
endif

MAKEOPTS = --no-print-directory Q=$(Q)

# build all by default
progs = $(progs_install) btrfsck btrfs-corrupt-block btrfs-calc-size

# install only selected
progs_install = btrfs mkfs.btrfs btrfs-debug-tree \
	btrfs-map-logical btrfs-image btrfs-zero-log \
	btrfs-find-root btrfstune btrfs-show-super \
	btrfs-select-super

progs_extra = btrfs-fragments

progs_static = $(foreach p,$(progs),$(p).static)

ifneq ($(DISABLE_BTRFSCONVERT),1)
progs_install += btrfs-convert
endif

# external libs required by various binaries; for btrfs-foo,
# specify btrfs_foo_libs = <list of libs>; see $($(subst...)) rules below
btrfs_convert_libs = -lext2fs  -lcom_err 
btrfs_convert_cflags = -DBTRFSCONVERT_EXT2=$(BTRFSCONVERT_EXT2)
btrfs_fragments_libs = -lgd -lpng -ljpeg -lfreetype
btrfs_debug_tree_objects = cmds-inspect-dump-tree.o
btrfs_show_super_objects = cmds-inspect-dump-super.o
btrfs_calc_size_objects = cmds-inspect-tree-stats.o

# collect values of the variables above
standalone_deps = $(foreach dep,$(patsubst %,%_objects,$(subst -,_,$(filter btrfs-%, $(progs)))),$($(dep)))

SUBDIRS =
BUILDDIRS = $(patsubst %,build-%,$(SUBDIRS))
INSTALLDIRS = $(patsubst %,install-%,$(SUBDIRS))
CLEANDIRS = $(patsubst %,clean-%,$(SUBDIRS))

ifneq ($(DISABLE_DOCUMENTATION),1)
BUILDDIRS += build-Documentation
INSTALLDIRS += install-Documentation
endif

.PHONY: $(SUBDIRS)
.PHONY: $(BUILDDIRS)
.PHONY: $(INSTALLDIRS)
.PHONY: $(TESTDIRS)
.PHONY: $(CLEANDIRS)
.PHONY: all install clean

# Create all the static targets
static_objects = $(patsubst %.o, %.static.o, $(objects))
static_cmds_objects = $(patsubst %.o, %.static.o, $(cmds_objects))
static_libbtrfs_objects = $(patsubst %.o, %.static.o, $(libbtrfs_objects))

libs_shared = libbtrfs.so.0.1
libs_static = libbtrfs.a
libs = $(libs_shared) $(libs_static)
lib_links = libbtrfs.so.0 libbtrfs.so
headers = $(libbtrfs_headers)

# make C=1 to enable sparse
ifdef C
	# We're trying to use sparse against glibc headers which go wild
	# trying to use internal compiler macros to test features.  We
	# copy gcc's and give them to sparse.  But not __SIZE_TYPE__
	# 'cause sparse defines that one.
	#
	dummy := $(shell $(CC) -dM -E -x c - < /dev/null | \
			grep -v __SIZE_TYPE__ > $(check_defs))
	check = $(CHECKER)
	check_echo = echo
else
	check = true
	check_echo = true
endif

%.o.d: %.c
	$(Q)$(CC) -MM -MG -MF $@ -MT $(@:.o.d=.o) -MT $(@:.o.d=.static.o) -MT $@ $(CFLAGS) $<

#
# Pick from per-file variables, btrfs_*_cflags
#
.c.o:
	@$(check_echo) "    [SP]     $<"
	$(Q)$(check) $(CFLAGS) $(CHECKER_FLAGS) $<
	@echo "    [CC]     $@"
	$(Q)$(CC) $(CFLAGS) -c $< $($(subst -,_,$(@:%.o=%)-cflags))

%.static.o: %.c
	@echo "    [CC]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -c $< -o $@ $($(subst -,_,$(@:%.static.o=%)-cflags))

all: $(progs) $(BUILDDIRS)
$(SUBDIRS): $(BUILDDIRS)
$(BUILDDIRS):
	@echo "Making all in $(patsubst build-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst build-%,%,$@)

test-convert: btrfs btrfs-convert
	@echo "    [TEST]   convert-tests.sh"
	$(Q)bash tests/convert-tests.sh

test-fsck: btrfs btrfs-image btrfs-corrupt-block btrfs-debug-tree mkfs.btrfs
	@echo "    [TEST]   fsck-tests.sh"
	$(Q)bash tests/fsck-tests.sh

test-misc: btrfs btrfs-image btrfs-corrupt-block btrfs-debug-tree mkfs.btrfs btrfstune
	@echo "    [TEST]   misc-tests.sh"
	$(Q)bash tests/misc-tests.sh

test-mkfs: btrfs mkfs.btrfs
	@echo "    [TEST]   mkfs-tests.sh"
	$(Q)bash tests/mkfs-tests.sh

test-fuzz: btrfs
	@echo "    [TEST]   fuzz-tests.sh"
	$(Q)bash tests/fuzz-tests.sh

test-cli: btrfs
	@echo "    [TEST]   cli-tests.sh"
	$(Q)bash tests/cli-tests.sh

test-clean:
	@echo "Cleaning tests"
	$(Q)bash tests/clean-tests.sh

test-inst: all
	@tmpdest=`mktemp --tmpdir -d btrfs-inst.XXXXXX` && \
		echo "Test installation to $$tmpdest" && \
		$(MAKE) DESTDIR=$$tmpdest install && \
		$(RM) -rf -- $$tmpdest

test: test-fsck test-mkfs test-convert test-misc test-fuzz

#
# NOTE: For static compiles, you need to have all the required libs
# 	static equivalent available
#
static: $(progs_static)

version.h: version.sh version.h.in configure.ac
	@echo "    [SH]     $@"
	$(Q)bash ./config.status --silent $@

$(libs_shared): $(libbtrfs_objects) $(lib_links) send.h
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) $(libbtrfs_objects) $(LDFLAGS) $(LIBBTRFS_LIBS) \
		-shared -Wl,-soname,libbtrfs.so.0 -o libbtrfs.so.0.1

$(libs_static): $(libbtrfs_objects)
	@echo "    [AR]     $@"
	$(Q)$(AR) cr libbtrfs.a $(libbtrfs_objects)

$(lib_links):
	@echo "    [LN]     $@"
	$(Q)$(LN_S) -f libbtrfs.so.0.1 $@

# keep intermediate files from the below implicit rules around
.PRECIOUS: $(addsuffix .o,$(progs))

# Make any btrfs-foo out of btrfs-foo.o, with appropriate libs.
# The $($(subst...)) bits below takes the btrfs_*_libs definitions above and
# turns them into a list of libraries to link against if they exist
#
# For static variants, use an extra $(subst) to get rid of the ".static"
# from the target name before translating to list of libs

btrfs-%.static: $(static_objects) btrfs-%.static.o $(static_libbtrfs_objects) $(patsubst %.o,%.static.o,$(standalone_deps))
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o $@ $@.o $(static_objects) \
		$(patsubst %.o, %.static.o, $($(subst -,_,$(subst .static,,$@)-objects))) \
		$(static_libbtrfs_objects) $(STATIC_LDFLAGS) \
		$($(subst -,_,$(subst .static,,$@)-libs)) $(STATIC_LIBS)

btrfs-%: $(objects) $(libs_static) btrfs-%.o $(standalone_deps)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o $@ $(objects) $@.o \
		$($(subst -,_,$@-objects)) \
		$(libs_static) \
		$(LDFLAGS) $(LIBS) $($(subst -,_,$@-libs))

btrfs: $(objects) btrfs.o $(cmds_objects) $(libs_static)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfs btrfs.o $(cmds_objects) \
		$(objects) $(libs_static) $(LDFLAGS) $(LIBS)

btrfs.static: $(static_objects) btrfs.static.o $(static_cmds_objects) $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o btrfs.static btrfs.static.o $(static_cmds_objects) \
		$(static_objects) $(static_libbtrfs_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS)

# For backward compatibility, 'btrfs' changes behaviour to fsck if it's named 'btrfsck'
btrfsck: btrfs
	@echo "    [LN]     $@"
	$(Q)$(LN_S) -f btrfs btrfsck

btrfsck.static: btrfs.static
	@echo "    [LN]     $@"
	$(Q)$(LN_S) -f $^ $@

mkfs.btrfs: $(objects) $(libs_static) mkfs.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o mkfs.btrfs $(objects) $(libs_static) mkfs.o $(LDFLAGS) $(LIBS)

mkfs.btrfs.static: $(static_objects) mkfs.static.o $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o mkfs.btrfs.static mkfs.static.o $(static_objects) \
		$(static_libbtrfs_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS)

btrfstune: $(objects) $(libs_static) btrfstune.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o btrfstune $(objects) btrfstune.o $(libs_static) $(LDFLAGS) $(LIBS)

btrfstune.static: $(static_objects) btrfstune.static.o $(static_libbtrfs_objects)
	@echo "    [LD]     $@"
	$(Q)$(CC) $(STATIC_CFLAGS) -o $@ btrfstune.static.o $(static_objects) \
		$(static_libbtrfs_objects) $(STATIC_LDFLAGS) $(STATIC_LIBS)

dir-test: $(objects) $(libs) dir-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o dir-test $(objects) $(libs) dir-test.o $(LDFLAGS) $(LIBS)

quick-test: $(objects) $(libs) quick-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o quick-test $(objects) $(libs) quick-test.o $(LDFLAGS) $(LIBS)

ioctl-test: $(objects) $(libs) ioctl-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o ioctl-test $(objects) $(libs) ioctl-test.o $(LDFLAGS) $(LIBS)

send-test: $(objects) $(libs) send-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o send-test $(objects) $(libs) send-test.o $(LDFLAGS) $(LIBS)

library-test: $(libs_shared) library-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o library-test library-test.o $(LDFLAGS) -lbtrfs

library-test.static: $(libs_static) library-test.o
	@echo "    [LD]     $@"
	$(Q)$(CC) $(CFLAGS) -o library-test-static library-test.o $(LDFLAGS) $(libs_static)

test-build: test-build-pre test-build-real

test-build-pre:
	$(MAKE) clean-all
	./autogen.sh
	./configure

test-build-real:
	$(MAKE) library-test
	-$(MAKE) library-test.static
	$(MAKE) -j 8 all
	-$(MAKE) -j 8 static
	$(MAKE) -j 8 $(progs_extra)

manpages:
	$(Q)$(MAKE) $(MAKEOPTS) -C Documentation


clean-all: clean clean-doc clean-gen

clean: $(CLEANDIRS)
	@echo "Cleaning"
	$(Q)$(RM) -f $(progs) cscope.out *.o *.o.d \
	      dir-test ioctl-test quick-test send-test library-test library-test-static \
	      btrfs.static mkfs.btrfs.static \
	      $(check_defs) \
	      $(libs) $(lib_links) \
	      $(progs_static) $(progs_extra)

clean-doc:
	@echo "Cleaning Documentation"
	$(Q)$(MAKE) $(MAKEOPTS) -C Documentation clean

clean-gen:
	@echo "Cleaning Generated Files"
	$(Q)$(RM) -rf version.h config.status config.cache connfig.log \
		configure.lineno config.status.lineno Makefile \
		Documentation/Makefile \
		config.log config.h config.h.in~ aclocal.m4 \
		configure autom4te.cache/ config/

$(CLEANDIRS):
	@echo "Cleaning $(patsubst clean-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst clean-%,%,$@) clean

install: $(libs) $(progs_install) $(INSTALLDIRS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs_install) $(DESTDIR)$(bindir)
	$(INSTALL) fsck.btrfs $(DESTDIR)$(bindir)
	# btrfsck is a link to btrfs in the src tree, make it so for installed file as well
	$(LN_S) -f btrfs $(DESTDIR)$(bindir)/btrfsck
	$(INSTALL) -m755 -d $(DESTDIR)$(libdir)
	$(INSTALL) $(libs) $(DESTDIR)$(libdir)
	cp -a $(lib_links) $(DESTDIR)$(libdir)
	$(INSTALL) -m755 -d $(DESTDIR)$(incdir)
	$(INSTALL) -m644 $(headers) $(DESTDIR)$(incdir)
ifneq ($(udevdir),)
	$(INSTALL) -m755 -d $(DESTDIR)$(udevruledir)
	$(INSTALL) -m644 $(udev_rules) $(DESTDIR)$(udevruledir)
endif

install-static: $(progs_static) $(INSTALLDIRS)
	$(INSTALL) -m755 -d $(DESTDIR)$(bindir)
	$(INSTALL) $(progs_static) $(DESTDIR)$(bindir)
	# btrfsck is a link to btrfs in the src tree, make it so for installed file as well
	$(LN_S) -f btrfs.static $(DESTDIR)$(bindir)/btrfsck.static

$(INSTALLDIRS):
	@echo "Making install in $(patsubst install-%,%,$@)"
	$(Q)$(MAKE) $(MAKEOPTS) -C $(patsubst install-%,%,$@) install

uninstall:
	$(Q)$(MAKE) $(MAKEOPTS) -C Documentation uninstall
	cd $(DESTDIR)$(incdir); $(RM) -f $(headers)
	$(RMDIR) -p --ignore-fail-on-non-empty $(DESTDIR)$(incdir)
	cd $(DESTDIR)$(libdir); $(RM) -f $(lib_links) $(libs)
	cd $(DESTDIR)$(bindir); $(RM) -f btrfsck fsck.btrfs $(progs_install)

ifneq ($(MAKECMDGOALS),clean)
-include $(objects:.o=.o.d) $(cmds_objects:.o=.o.d) $(subst .btrfs,, $(filter-out btrfsck.o.d, $(progs:=.o.d)))
endif
