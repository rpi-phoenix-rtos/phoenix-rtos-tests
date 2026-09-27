#
# Makefile for mem tests
#
# Copyright 2019 Phoenix Systems
#
# %LICENSE%
#

# LEGACY 
$(eval $(call add_test, test_mmap))
$(eval $(call add_test, test_malloc))

# UNITY

NAME := test-mprotect
LOCAL_SRCS := test_mprotect.c
DEP_LIBS := unity

include $(binary.mk)

NAME := test-mprotect-fault
LOCAL_SRCS := test_mprotect_fault.c
DEP_LIBS := unity

include $(binary.mk)

# Not in test.yaml: group msg_devmem faults kernels without the proc/msg.c
# device-payload fix for ever (see the header of test_msg_devmem.c)
NAME := test-msg-devmem
LOCAL_SRCS := test_msg_devmem.c
DEP_LIBS := unity

include $(binary.mk)

$(eval $(call add_unity_test, test_mmap_new))
