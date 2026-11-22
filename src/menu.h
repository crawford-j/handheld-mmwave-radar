/*
 * menu.h - Touch settings menu (stylus-friendly), opened from the radar view.
 */

#ifndef MENU_H
#define MENU_H

/* Called repeatedly while the menu is open, so the caller can keep reading
 * the radar (otherwise its bytes pile up in the ring buffer). */
typedef void (*menu_idle_fn)(void);

void menu_run(menu_idle_fn idle);

#endif /* MENU_H */
