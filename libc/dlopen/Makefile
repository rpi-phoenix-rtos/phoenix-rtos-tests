#
# Makefile for the dlopen() tests
#
# test-libc-dlopen is a static program with an export table: linked with
# --no-dynamic-linker and --dynamic-list, ld gives it a dynamic symbol table holding
# exactly the symbols in exports.list (and keeps them under --gc-sections). The
# plugins are -fPIC shared objects that leave those symbols undefined for dlopen()
# to bind. -nostartfiles because the toolchain's startfiles are the program's
# (crt0 with _start); -nostdlib so that no second libc is linked into a plugin.
#
# libphoenix's loader handles aarch64 objects only.
#
# Copyright 2026 Phoenix Systems
#
# %LICENSE%
#

ifneq ($(filter aarch64%,$(TARGET_FAMILY)),)

DLOPEN_DIR := $(call my-dir)

NAME := test-libc-dlopen
LOCAL_SRCS := dlopen.c main.c
DEP_LIBS := unity
LOCAL_LDFLAGS := -Wl,--no-dynamic-linker -Wl,--dynamic-list=$(abspath $(DLOPEN_DIR)exports.list)

include $(binary.mk)

# $(eval $(call add_dlopen_plugin,NAME,HASH_STYLE))
define add_dlopen_plugin
NAME := test-libc-dlopen-$(1).so
LOCAL_SRCS := plugins/$(1).c
LOCAL_CFLAGS := -fPIC
LOCAL_LDFLAGS := -shared -nostartfiles -nostdlib -Wl,--hash-style=$(2)

include $(binary.mk)
endef

$(eval $(call add_dlopen_plugin,import,sysv))
$(eval $(call add_dlopen_plugin,weak,sysv))
$(eval $(call add_dlopen_plugin,gnuhash,gnu))
$(eval $(call add_dlopen_plugin,tls,sysv))

endif
