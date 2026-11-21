#ifndef JPT_CE_GAMEPLAY_H
#define JPT_CE_GAMEPLAY_H

void ce_gameplay_configure(int dual_stow, int max_object_detail);
void ce_gameplay_install(void);
/* Where the game could call it itself (its UI loop, a presented frame): Maximum object detail and the collision sounds. */
void ce_gameplay_step(void);

#endif
