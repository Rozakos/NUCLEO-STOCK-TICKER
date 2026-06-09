#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

/**
 * Initialize external SDRAM and draw a framebuffer test pattern.
 *
 * Returns 0 on success, -1 if SDRAM initialization or verification fails.
 */
int display_init(void);

#endif /* APP_DISPLAY_H */
