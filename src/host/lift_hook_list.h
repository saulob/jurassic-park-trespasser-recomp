/*
 * Host hooks inside lifted functions, sorted by guest address. tools/run_lift.py puts RECOMP_LIFT_HOOK(lift_hook_<va>)
 * in front of the instruction at each address, in every lifted body that holds it, <va> being this list's address in every
 * lift; the host defines lift_hook_<va> once.
 * A hook runs with the guest's registers, flags and memory and may change them; the instruction then runs as lifted.
 */
LIFT_HOOK(00441D1D) /* player tuning built, pick-up hold time (patch117_player.c) */
LIFT_HOOK(0044C1B2) /* Move, the view and the raised hand (patch117_player.c) */
LIFT_HOOK(0044CF9F) /* CPlayer::Process, grab gate (patch117_player.c) */
LIFT_HOOK(00451212) /* CPlayer::Process, after a pick-up (patch117_player.c) */
LIFT_HOOK(0045125D) /* CPlayer::Process, hand raise (patch117_player.c) */
LIFT_HOOK(004512FF) /* CPlayer::Process, 116 raising step (patch117_player.c) */
LIFT_HOOK(004519A1) /* CPlayer::Process, hand steering gate (patch117_player.c) */
LIFT_HOOK(004546D7) /* CPlayer::Process, mouse look body turn (patch117_player.c) */
LIFT_HOOK(004546E2) /* CPlayer::Process, mouse look body turn (patch117_player.c) */
LIFT_HOOK(0045472E) /* CPlayer::Process, mouse look body turn (patch117_player.c) */
LIFT_HOOK(00454734) /* CPlayer::Process, mouse look body turn (patch117_player.c) */
LIFT_HOOK(00455B6B) /* CPlayer::Process, held jump key (patch117_player.c) */
LIFT_HOOK(004569C8) /* player impulse, jump lifetime (patch117_player.c) */
LIFT_HOOK(0045B916) /* hand reach, empty hand (patch117_player.c) */
LIFT_HOOK(0045B91C) /* hand reach, empty hand (patch117_player.c) */
LIFT_HOOK(00460CF5) /* player impulse, jump lifetime (patch117_player.c) */
LIFT_HOOK(00461491) /* stow handler, empty-hand reach (patch117_player.c) */
LIFT_HOOK(004DDEFD) /* broad phase, ignore-collision pair wake (patch117_physics.c) */
LIFT_HOOK(004E78D2) /* Foot_IDOF, failed jump ground check (patch117_player.c) */
LIFT_HOOK(004EB46B) /* box-box +z velocity kill normal (patch117_physics.c) */
LIFT_HOOK(004EB781) /* box-box -z velocity kill normal (patch117_physics.c) */
LIFT_HOOK(004EBA7B) /* box-box +x velocity kill normal (patch117_physics.c) */
LIFT_HOOK(004EBD85) /* box-box -x velocity kill normal (patch117_physics.c) */
LIFT_HOOK(004EC07C) /* box-box +y velocity kill normal (patch117_physics.c) */
LIFT_HOOK(004EC386) /* box-box -y velocity kill normal (patch117_physics.c) */
LIFT_HOOK(004EC586) /* box-box, a corner already inside (patch117_physics.c) */
LIFT_HOOK(004EC5A2) /* box-box edge-edge gate (patch117_physics.c) */
