# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2019 Western Digital Corporation or its affiliates

UNAME := $(shell uname -s)

ifeq ($(UNAME),FreeBSD)
export CC := cc
else
export CC := $(CROSS_COMPILE)gcc
endif
AM_CFLAGS = -D_FILE_OFFSET_BITS=64 -D_FORTIFY_SOURCE=2
CFLAGS ?= -g -O2 -static -D_GNU_SOURCE

ifneq ($(CROSS_COMPILE),)
	LDFLAGS += -static
endif

#CXXFLAGS = -DDEBUG

objects = \
	ufs.o \
	ufs_cmds.o \
	options.o \
	scsi_bsg_util.o \
	ufs_err_hist.o \
	unipro.o \
	ufs_ffu.o \
	ufs_vendor.o\
	hmac_sha2.o \
	sha2.o \
	ufs_rpmb.o \
	ufs_arpmb.o \
	ufs_hmr.o \
	ufs_emon.o \

ifeq ($(UNAME),FreeBSD)
objects += freebsd_transport.o
# compat carries the Linux headers ufs-utils includes, and a staged copy
# of the ufshci headers until they ship in /usr/include.
INC_DIR += -I$(CURDIR)/compat

# Stage the ufshci headers so a fresh checkout builds. Probe for the ioctl
# header itself: a base system may ship ufshci.h before it ships that one,
# and staging a mismatched pair changes the ioctl encoding.
STAGE_DIR := $(CURDIR)/compat/dev/ufshci
ifeq ($(origin UFSHCI_HDR_SRC),undefined)
UFSHCI_HDR_SRC := $(dir $(firstword $(wildcard \
        /usr/include/dev/ufshci/ufshci_ioctl.h \
        /usr/src/sys/dev/ufshci/ufshci_ioctl.h)))
endif
STAGED_HDRS := $(STAGE_DIR)/ufshci.h $(STAGE_DIR)/ufshci_ioctl.h
# options.h defines PATH_MAX itself unless a Linux guard macro says the
# system already has one. Pull in the real limits.h and set that guard,
# so the platform value is used instead of a second definition.
AM_CFLAGS += -include limits.h -D_UAPI_LINUX_LIMITS_H
endif

CHECKFLAGS = -Wall  -Wundef -Wno-missing-braces -fcommon

DEPFLAGS = -Wp,-MMD,$(@D)/.$(@F).d,-MT,$@
override CFLAGS := $(CHECKFLAGS) $(AM_CFLAGS) $(CFLAGS) $(INC_DIR) $(CXXFLAGS)
progs = ufs-utils
ifdef C
	check = sparse $(CHECKFLAGS)
endif

.c.o:
ifdef C
	$(check) $<
endif
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

ufs-utils:$(objects)
	$(CC) $(CFLAGS) -o $@ $(objects) $(LDFLAGS) $(LIBS)

help:
	@echo "\033[31m==============Build Instructions==============\033[0m"
	@echo "\033[92mTo build ufs_utils follow the following steps\033[0m"
	@echo "\033[92m1 Set CROSS_COMPILE variable\033[0m"
	@echo "\033[92m2 Build the tool using \"make\"\033[0m"
	@echo "\033[92m3 Clean the tool using \"make clean\"\033[0m"

clean:
	@rm -f $(progs) $(objects) .*.o.d
	@rm -rf $(CURDIR)/compat/dev
.PHONY: all clean

# Staging rules live here so they cannot take over as the default goal.
ifeq ($(UNAME),FreeBSD)
ifeq ($(UFSHCI_HDR_SRC),)
$(STAGED_HDRS):
	@echo "ufshci headers not found." >&2
	@echo "Set UFSHCI_HDR_SRC to a directory holding ufshci.h and" >&2
	@echo "ufshci_ioctl.h, for example /usr/src/sys/dev/ufshci/." >&2
	@false
else
$(STAGE_DIR)/.source: FORCE
	@mkdir -p $(@D)
	@echo $(UFSHCI_HDR_SRC) | cmp -s - $@ || \
		echo $(UFSHCI_HDR_SRC) > $@

$(STAGE_DIR)/%.h: $(patsubst %/,%,$(UFSHCI_HDR_SRC))/%.h $(STAGE_DIR)/.source
	@mkdir -p $(@D)
	cp -f $< $@
endif

FORCE:
.PHONY: FORCE

$(objects): $(STAGED_HDRS)
endif
