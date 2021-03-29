# Makefile for backfill scheduler plugin

AUTOMAKE_OPTIONS = foreign

AM_CXXFLAGS = -fexceptions

PLUGIN_FLAGS = -module -avoid-version --export-dynamic

AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/src/common -I/usr/include/python2.7 -I/usr/include

PLUGIN_FLAGS += -lpthread -ldl -lutil -lm -lpython2.7

pkglib_LTLIBRARIES = sched_backfill.la

sched_backfill_la_SOURCES = backfill_wrapper.c	\
			backfill.c	\
			backfill.h
sched_backfill_la_LDFLAGS = $(PLUGIN_FLAGS)
