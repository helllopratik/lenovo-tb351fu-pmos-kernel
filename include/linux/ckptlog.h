/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CKPTLOG_H
#define _LINUX_CKPTLOG_H

void ckpt_checkpoint(const char *stage);
void fb_klog_start(void);
void fb_text_panic_dump(void);

#endif
