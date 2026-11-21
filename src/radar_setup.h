/*
 * radar_setup.h - One-time routine that moves the LD2450 from its factory
 * 256000 baud to 38400. Built only with `make RADAR_SETUP=1`.
 */

#ifndef RADAR_SETUP_H
#define RADAR_SETUP_H

void radar_setup_run(void) __attribute__((noreturn));

#endif /* RADAR_SETUP_H */
