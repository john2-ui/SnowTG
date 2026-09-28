# Shared build-time selection; public struct layouts do not depend on this flag.
OWNER_TIMER_BACKEND ?= rte
ifeq ($(OWNER_TIMER_BACKEND),rte)
OWNER_TIMER_CFLAGS := -DOWNER_TIMER_WHEEL=0
else ifeq ($(OWNER_TIMER_BACKEND),wheel)
OWNER_TIMER_CFLAGS := -DOWNER_TIMER_WHEEL=1
else
$(error OWNER_TIMER_BACKEND must be rte or wheel)
endif
