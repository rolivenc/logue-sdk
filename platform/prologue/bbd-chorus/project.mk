#
# Project configuration for the logue SDK build system (prologue)
#
PROJECT = bbd_chorus

UCSRC =

UCXXSRC = bbd_chorus.cpp

UINCDIR =

# prologue is dual-timbre: process main + sub in parallel
UDEFS = -DDUAL_TIMBRE=1

ULIB = -lm

ULIBDIR =
