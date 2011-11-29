/***************************************************************************
 *
 * Multitouch X driver
 * Copyright (C) 2008 Henrik Rydberg <rydberg@euromail.se>
 * Copyright (C) 2011 Ryan Bourgeois <bluedragonx@gmail.com>
 *
 * Gestures
 * Copyright (C) 2008 Henrik Rydberg <rydberg@euromail.se>
 * Copyright (C) 2010 Arturo Castro <mail@arturocastro.net>
 * Copyright (C) 2011 Ryan Bourgeois <bluedragonx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 **************************************************************************/

#include "gestures.h"
#include "trig.h"
#include <poll.h>

#define IS_VALID_BUTTON(x) (x >= 0 && x <= 31)
#define SPEED(d, t) ((t) == 0 ? 0 : ((double)(d))/((double)(t)))

static void trigger_button_up(struct Gestures* gs, int button)
{
	if (IS_VALID_BUTTON(button)) {
		if (button == 0 && gs->button_emulate > 0) {
			button = gs->button_emulate;
			gs->button_emulate = 0;
		}
		CLEARBIT(gs->buttons, button);
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_button_up: %d up\n", button);
#endif
	}
}

static void trigger_button_down(struct Gestures* gs, int button)
{
	if (IS_VALID_BUTTON(button) && button != gs->delayed_click_button) {
		SETBIT(gs->buttons, button);
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_button_down: %d down\n", button);
#endif
	}
#ifdef DEBUG_GESTURES
	else if (IS_VALID_BUTTON(button))
		xf86Msg(X_INFO, "trigger_button_down: %d down ignored, in delayed mode\n", button);
#endif
}

static void trigger_button_emulation(struct Gestures* gs, int button)
{
	if (IS_VALID_BUTTON(button) && GETBIT(gs->buttons, 0)) {
		CLEARBIT(gs->buttons, 0);
		SETBIT(gs->buttons, button);
		gs->button_emulate = button;
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_button_emulation: %d emulated\n", button);
#endif
	}
}

static void trigger_button_click(struct Gestures* gs,
			int button, mstime_t trigger_up_time)
{
	if (IS_VALID_BUTTON(button) && gs->delayed_click_button == -1) {
		trigger_button_down(gs, button);
		gs->delayed_click_button = button;
		gs->delayed_click_wake = trigger_up_time;
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_button_click: %d placed in delayed mode\n", button);
#endif
	}
#ifdef DEBUG_GESTURES
	else if (IS_VALID_BUTTON(button))
		xf86Msg(X_INFO, "trigger_button_click: %d ignored, in delayed mode\n", button);
#endif
}

static void trigger_drag_ready(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs)
{
	gs->move_drag = GS_DRAG_READY;
	gs->move_drag_expire = hs->evtime + cfg->drag_timeout;
#ifdef DEBUG_GESTURES
	xf86Msg(X_INFO, "trigger_drag_ready: drag is ready\n");
#endif
}

static int trigger_drag_start(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			int dx, int dy)
{
	if (gs->move_drag == GS_DRAG_READY) {
		gs->move_drag_expire = 0;
		if (cfg->drag_wait == 0) {
 			gs->move_drag = GS_DRAG_ACTIVE;
			trigger_button_down(gs, 0);
#ifdef DEBUG_GESTURES
			xf86Msg(X_INFO, "trigger_drag_start: drag is active\n");
#endif
		}
		else {
			gs->move_drag = GS_DRAG_WAIT;
			gs->move_drag_wait = hs->evtime + cfg->drag_wait;
			gs->move_drag_dx = dx;
			gs->move_drag_dy = dy;
#ifdef DEBUG_GESTURES
			xf86Msg(X_INFO, "trigger_drag_start: drag in wait\n");
#endif
		}
	}
	else if (gs->move_drag == GS_DRAG_WAIT) {
		gs->move_drag_dx += dx;
		gs->move_drag_dy += dy;
		if (hs->evtime >= gs->move_drag_wait) {
			gs->move_drag = GS_DRAG_ACTIVE;
			trigger_button_down(gs, 0);
#ifdef DEBUG_GESTURES
			xf86Msg(X_INFO, "trigger_drag_start: drag is active\n");
#endif
		}
		else if (dist2(gs->move_drag_dx, gs->move_drag_dy) > SQRVAL(cfg->drag_dist)) {
			gs->move_drag = GS_NONE;
#ifdef DEBUG_GESTURES
			xf86Msg(X_INFO, "trigger_drag_start: drag canceled, moved too far\n");
#endif
		}
	}
	return gs->move_drag != GS_DRAG_WAIT;
}

static void trigger_drag_stop(struct Gestures* gs, int force)
{
	if (gs->move_drag == GS_DRAG_READY && force) {
		gs->move_drag = GS_NONE;
		gs->move_drag_expire = 0;
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_drag_stop: drag canceled\n");
#endif
	}
	else if (gs->move_drag == GS_DRAG_ACTIVE) {
		gs->move_drag = GS_NONE;
		gs->move_drag_expire = 0;
		trigger_button_up(gs, 0);
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_drag_stop: drag stopped\n");
#endif
	}
}

static void buttons_update(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			struct MTState* ms)
{
	if (!cfg->button_enable || cfg->trackpad_disable >= 3)
		return;

	static bitmask_t button_prev = 0U;
	int i, down, emulate, touching;
	down = 0;
	emulate = GETBIT(hs->button, 0) && !GETBIT(button_prev, 0);

	for (i = 0; i < 32; i++) {
		if (GETBIT(hs->button, i) == GETBIT(button_prev, i))
			continue;
		if (GETBIT(hs->button, i)) {
			down++;
			trigger_button_down(gs, i);
		}
		else
			trigger_button_up(gs, i);
	}
	button_prev = hs->button;

	if (down) {
		int earliest, latest, moving = 0;
		gs->move_type = GS_NONE;
		gs->move_wait = hs->evtime + cfg->gesture_wait;
		earliest = -1;
		latest = -1;
		foreach_bit(i, ms->touch_used) {
			if (GETBIT(ms->touch[i].state, MT_INVALID))
				continue;
			if (cfg->button_integrated && !GETBIT(ms->touch[i].flags, GS_BUTTON))
				SETBIT(ms->touch[i].flags, GS_BUTTON);
			if (earliest == -1 || ms->touch[i].down < ms->touch[earliest].down)
				earliest = i;
			if (latest == -1 || ms->touch[i].down > ms->touch[latest].down)
				latest = i;
		}

		if (emulate) {
			if (cfg->button_zones && earliest >= 0) {
				int zones, left, right, pos;
				double width;

				zones = 0;
				if (cfg->button_1touch > 0)
					zones++;
				if (cfg->button_2touch > 0)
					zones++;
				if (cfg->button_3touch > 0)
					zones++;

				if (zones > 0) {
					width = ((double)cfg->pad_width)/((double)zones);
					pos = cfg->pad_width / 2 + ms->touch[earliest].x;
#ifdef DEBUG_GESTURES
					xf86Msg(X_INFO, "buttons_update: pad width %d, zones %d, zone width %f, x %d\n",
						cfg->pad_width, zones, width, pos);
#endif
					for (i = 0; i < zones; i++) {
						left = width*i;
						right = width*(i+1);
						if (pos >= left && pos <= right) {
#ifdef DEBUG_GESTURES
							xf86Msg(X_INFO, "buttons_update: button %d, left %d, right %d (found)\n", i, left, right);
#endif
							break;
						}
#ifdef DEBUG_GESTURES
						else
							xf86Msg(X_INFO, "buttons_update: button %d, left %d, right %d\n", i, left, right);
#endif
					}

					if (i == 0)
						trigger_button_emulation(gs, cfg->button_1touch - 1);
					else if (i == 1)
						trigger_button_emulation(gs, cfg->button_2touch - 1);
					else
						trigger_button_emulation(gs, cfg->button_3touch - 1);
				}
			}
			else if (latest >= 0) {
				touching = 0;
				foreach_bit(i, ms->touch_used) {
					if (cfg->button_move || cfg->button_expire == 0 || ms->touch[latest].down < ms->touch[i].down + cfg->button_expire)
						touching++;
				}

				if (cfg->button_integrated)
					touching--;

				if (touching == 1 && cfg->button_1touch > 0)
					trigger_button_emulation(gs, cfg->button_1touch - 1);
				else if (touching == 2 && cfg->button_2touch > 0)
					trigger_button_emulation(gs, cfg->button_2touch - 1);
				else if (touching == 3 && cfg->button_3touch > 0)
					trigger_button_emulation(gs, cfg->button_3touch - 1);
			}
		}
	}
}

static void tapping_update(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			struct MTState* ms)
{
	int i, n, dist, released_max;

	if (cfg->trackpad_disable >= 1)
		return;

	if (cfg->tap_4touch > 0)
		released_max = 4;
	else if (cfg->tap_3touch > 0)
		released_max = 3;
	else if (cfg->tap_2touch > 0)
		released_max = 2;
	else if (cfg->tap_1touch > 0)
		released_max = 1;
	else
		return;

	if (gs->tap_time_down != 0 && hs->evtime >= gs->tap_time_down + cfg->tap_timeout) {
		gs->tap_time_down = 0;
		gs->tap_touching = 0;
		gs->tap_released = 0;
		foreach_bit(i, ms->touch_used) {
			if (GETBIT(ms->touch[i].flags, GS_TAP))
				CLEARBIT(ms->touch[i].flags, GS_TAP);
		}
	}
	else {
		foreach_bit(i, ms->touch_used) {
			if (GETBIT(ms->touch[i].state, MT_INVALID) || GETBIT(ms->touch[i].flags, GS_BUTTON)) {
				if (GETBIT(ms->touch[i].flags, GS_TAP)) {
					CLEARBIT(ms->touch[i].flags, GS_TAP);
					gs->tap_touching--;
#ifdef DEBUG_GESTURES
					xf86Msg(X_INFO, "tapping_update: tap_touching-- (%d): invalid or button\n", gs->tap_touching);
#endif
				}
			}
			else {
				if (GETBIT(ms->touch[i].state, MT_NEW)) {
					SETBIT(ms->touch[i].flags, GS_TAP);
					gs->tap_touching++;
#ifdef DEBUG_GESTURES
					xf86Msg(X_INFO, "tapping_update: tap_touching++ (%d): new touch\n", gs->tap_touching);
#endif
					if (gs->tap_time_down == 0)
						gs->tap_time_down = hs->evtime;
				}

				if (GETBIT(ms->touch[i].flags, GS_TAP)) {
					dist = dist2(ms->touch[i].total_dx, ms->touch[i].total_dy);
					if (dist >= SQRVAL(cfg->tap_dist)) {
						CLEARBIT(ms->touch[i].flags, GS_TAP);
						gs->tap_touching--;
#ifdef DEBUG_GESTURES
					xf86Msg(X_INFO, "tapping_update: tap_touching-- (%d): moved too far\n", gs->tap_touching);
#endif
					}
					else if (GETBIT(ms->touch[i].state, MT_RELEASED)) {
						gs->tap_touching--;
						gs->tap_released++;
#ifdef DEBUG_GESTURES
					xf86Msg(X_INFO, "tapping_update: tap_touching-- (%d): released\n", gs->tap_touching);
					xf86Msg(X_INFO, "tapping_update: tap_released++ (%d) (max %d): released\n", gs->tap_released, released_max);
#endif
					}
				}
			}
		}
	}

	if ((gs->tap_touching == 0 && gs->tap_released > 0) || gs->tap_released >= released_max) {
		foreach_bit(i, ms->touch_used) {
			if (GETBIT(ms->touch[i].flags, GS_TAP))
				CLEARBIT(ms->touch[i].flags, GS_TAP);
		}

		if (gs->tap_released == 1)
			n = cfg->tap_1touch - 1;
		else if (gs->tap_released == 2)
			n = cfg->tap_2touch - 1;
		else if (gs->tap_released == 3)
			n = cfg->tap_3touch - 1;
		else
			n = cfg->tap_4touch - 1;

		trigger_button_click(gs, n, hs->evtime + cfg->tap_hold);
		if (cfg->drag_enable && n == 0)
			trigger_drag_ready(gs, cfg, hs);

		gs->move_type = GS_NONE;
		gs->move_wait = hs->evtime + cfg->gesture_wait;

		gs->tap_time_down = 0;
		gs->tap_touching = 0;
		gs->tap_released = 0;
	}
}

static void trigger_move(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			int dx, int dy)
{
	if ((gs->move_type == GS_MOVE || hs->evtime >= gs->move_wait) && (dx != 0 || dy != 0)) {
		if (trigger_drag_start(gs, cfg, hs, dx, dy)) {
			gs->move_dx = (int)(dx*cfg->sensitivity);
			gs->move_dy = (int)(dy*cfg->sensitivity);
			gs->move_type = GS_MOVE;
			gs->move_wait = 0;
			gs->move_dist = 0;
			gs->move_dir = TR_NONE;
#ifdef DEBUG_GESTURES
			xf86Msg(X_INFO, "trigger_move: %d, %d\n", dx, dy);
#endif
		}
	}
}

static int trigger_decel(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs)
{
	if ((gs->move_type == GS_SCROLL && cfg->scroll_coast_enable &&
			gs->move_speed >= cfg->scroll_coast_speed && cfg->scroll_coast_decel > 0) ||
			(gs->move_type == GS_SWIPE3 && cfg->swipe_coast_enable &&
			gs->move_speed >= cfg->swipe_coast_speed && cfg->swipe_coast_decel > 0) ||
			(gs->move_type == GS_SWIPE4 && cfg->swipe4_coast_enable &&
			gs->move_speed >= cfg->swipe4_coast_speed && cfg->swipe4_coast_decel > 0)) {
		gs->delayed_decel_wake = hs->evtime + GS_DECEL_TICK;
		gs->delayed_decel_speed = gs->move_speed;
#ifdef DEBUG_GESTURE
		xf86Msg(X_INFO, "trigger_decel: starting decel at speed %d\n", gs->delayed_decel_speed);
#endif
		return 1;
	}
#ifdef DEBUG_GESTURE
	else
		xf86Msg(X_INFO, "trigger_decel: skipping decel at speed %d\n", gs->delayed_decel_speed);
#endif
	return 0;
}

static void trigger_scroll(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			int dist, int dir)
{
	if (gs->move_type == GS_SCROLL || hs->evtime >= gs->move_wait) {
		trigger_drag_stop(gs, 1);
		if (gs->move_type != GS_SCROLL || gs->move_dir != dir)
			gs->move_dist = 0;
		gs->move_dx = 0;
		gs->move_dy = 0;
		gs->move_type = GS_SCROLL;
		gs->move_wait = hs->evtime + cfg->gesture_wait;
		gs->move_dist += ABSVAL(dist);
		gs->move_speed = SPEED(gs->move_dist, hs->deltat);
		gs->move_dir = dir;
		if (gs->move_dist >= cfg->scroll_dist) {
			gs->move_dist = MODVAL(gs->move_dist, cfg->scroll_dist);
			if (dir == TR_DIR_UP)
				trigger_button_click(gs, cfg->scroll_up_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_DN)
				trigger_button_click(gs, cfg->scroll_dn_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_LT)
				trigger_button_click(gs, cfg->scroll_lt_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_RT)
				trigger_button_click(gs, cfg->scroll_rt_btn - 1, hs->evtime + cfg->gesture_hold);
		}
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_scroll: scrolling %+d in direction %d (at %d of %d)\n", dist, dir, gs->move_dist, cfg->scroll_dist);
#endif
	}
}

static void trigger_swipe3(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			int dist, int dir)
{
	if (gs->move_type == GS_SWIPE3 || hs->evtime >= gs->move_wait) {
		trigger_drag_stop(gs, 1);
		if (gs->move_type != GS_SWIPE3 || gs->move_dir != dir)
			gs->move_dist = 0;
		gs->move_dx = 0;
		gs->move_dy = 0;
		gs->move_type = GS_SWIPE3;
		gs->move_wait = hs->evtime + cfg->gesture_wait;
		gs->move_dist += ABSVAL(dist);
		gs->move_speed = SPEED(gs->move_dist, hs->deltat);
		gs->move_dir = dir;

		if (cfg->swipe_dist > 0 && gs->move_dist >= cfg->swipe_dist) {
			gs->move_dist = MODVAL(gs->move_dist, cfg->swipe_dist);
			if (dir == TR_DIR_UP)
				trigger_button_click(gs, cfg->swipe_up_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_DN)
				trigger_button_click(gs, cfg->swipe_dn_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_LT)
				trigger_button_click(gs, cfg->swipe_lt_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_RT)
				trigger_button_click(gs, cfg->swipe_rt_btn - 1, hs->evtime + cfg->gesture_hold);
		}
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_swipe3: swiping %+d in direction %d (at %d of %d)\n", dist, dir, gs->move_dist, cfg->swipe_dist);
#endif
	}
}

static void trigger_swipe4(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			int dist, int dir)
{
	if (gs->move_type == GS_SWIPE4 || hs->evtime >= gs->move_wait) {
		trigger_drag_stop(gs, 1);
		if (gs->move_type != GS_SWIPE4 || gs->move_dir != dir)
			gs->move_dist = 0;
		gs->move_dx = 0;
		gs->move_dy = 0;
		gs->move_type = GS_SWIPE4;
		gs->move_wait = hs->evtime + cfg->gesture_wait;
		gs->move_dist += ABSVAL(dist);
		gs->move_speed = SPEED(gs->move_dist, hs->deltat);
		gs->move_dir = dir;
		
		if (cfg->swipe4_dist > 0 && gs->move_dist >= cfg->swipe4_dist) {
			gs->move_dist = MODVAL(gs->move_dist, cfg->swipe4_dist);
			if (dir == TR_DIR_UP)
				trigger_button_click(gs, cfg->swipe4_up_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_DN)
				trigger_button_click(gs, cfg->swipe4_dn_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_LT)
				trigger_button_click(gs, cfg->swipe4_lt_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_RT)
				trigger_button_click(gs, cfg->swipe4_rt_btn - 1, hs->evtime + cfg->gesture_hold);
		}
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_swipe4: swiping %+d in direction %d (at %d of %d)\n", dist, dir, gs->move_dist, cfg->swipe_dist);
#endif
	}
}

static void trigger_scale(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			int dist, int dir)
{
	if (gs->move_type == GS_SCALE || hs->evtime >= gs->move_wait) {
		int scale_dist_sqr = SQRVAL(cfg->scale_dist);
		trigger_drag_stop(gs, 1);
		if (gs->move_type != GS_SCALE || gs->move_dir != dir)
			gs->move_dist = 0;
		gs->move_dx = 0;
		gs->move_dy = 0;
		gs->move_type = GS_SCALE;
		gs->move_wait = hs->evtime + cfg->gesture_wait;
		gs->move_dist += ABSVAL(dist);
		gs->move_dir = dir;
		if (gs->move_dist >= scale_dist_sqr) {
			gs->move_dist = MODVAL(gs->move_dist, scale_dist_sqr);
			if (dir == TR_DIR_UP)
				trigger_button_click(gs, cfg->scale_up_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_DN)
				trigger_button_click(gs, cfg->scale_dn_btn - 1, hs->evtime + cfg->gesture_hold);
		}
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_scale: scaling %+d in direction %d (at %d of %d)\n", dist, dir, gs->move_dist, scale_dist_sqr);
#endif
	}
}

static void trigger_rotate(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			int dist, int dir)
{
	if (gs->move_type == GS_ROTATE || hs->evtime >= gs->move_wait) {
		int rotate_dist_sqr = SQRVAL(cfg->rotate_dist);
		trigger_drag_stop(gs, 1);
		if (gs->move_type != GS_ROTATE || gs->move_dir != dir)
			gs->move_dist = 0;
		gs->move_dx = 0;
		gs->move_dy = 0;
		gs->move_type = GS_ROTATE;
		gs->move_wait = hs->evtime + cfg->gesture_wait;
		gs->move_dist += ABSVAL(dist);
		gs->move_dir = dir;
		if (gs->move_dist >= rotate_dist_sqr) {
			gs->move_dist = MODVAL(gs->move_dist, rotate_dist_sqr);
			if (dir == TR_DIR_LT)
				trigger_button_click(gs, cfg->rotate_lt_btn - 1, hs->evtime + cfg->gesture_hold);
			else if (dir == TR_DIR_RT)
				trigger_button_click(gs, cfg->rotate_rt_btn - 1, hs->evtime + cfg->gesture_hold);
		}
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "trigger_rotate: rotating %+d in direction %d (at %d of %d)\n", dist, dir, gs->move_dist, rotate_dist_sqr);
#endif
	}
}

static void trigger_reset(struct Gestures* gs)
{
	trigger_drag_stop(gs, 0);
	gs->move_dx = 0;
	gs->move_dy = 0;
	gs->move_type = GS_NONE;
	gs->move_wait = 0;
	gs->move_dist = 0;
	gs->move_speed = 0;
	gs->move_dir = TR_NONE;
}

static int get_scroll_dir(const struct Touch* t1,
			const struct Touch* t2)
{
	if (trig_angles_acute(t1->direction, t2->direction) < 2.0)
		return trig_generalize(t1->direction);
	return TR_NONE;
}

static int get_rotate_dir(const struct Touch* t1,
			const struct Touch* t2)
{
	double v, d1, d2;
	v = trig_direction(t2->x - t1->x, t2->y - t1->y);
	d1 = trig_angles_add(v, 2);
	d2 = trig_angles_sub(v, 2);
	if (trig_angles_acute(t1->direction, d1) < 2 && trig_angles_acute(t2->direction, d2) < 2)
		return TR_DIR_RT;
	else if (trig_angles_acute(t1->direction, d2) < 2 && trig_angles_acute(t2->direction, d1) < 2)
		return TR_DIR_LT;
	return TR_NONE;
}

static int get_scale_dir(const struct Touch* t1,
			const struct Touch* t2)
{
	double v;
	if (trig_angles_acute(t1->direction, t2->direction) >= 2) {
		v = trig_direction(t2->x - t1->x, t2->y - t1->y);
		if (trig_angles_acute(v, t1->direction) < 2)
			return TR_DIR_DN;
		else
			return TR_DIR_UP;
	}
	return TR_NONE;
}

static int get_swipe_dir(const struct Touch* t1,
			const struct Touch* t2,
			const struct Touch* t3)
{
	double d1, d2;
	d1 = MINVAL(t1->direction, MINVAL(t2->direction, t3->direction));
	d2 = MAXVAL(t1->direction, MAXVAL(t2->direction, t3->direction));
	if (trig_angles_acute(d1, d2) < 2)
		return trig_generalize(t1->direction);
	return TR_NONE;
}

static int get_swipe4_dir(const struct Touch* t1,
			const struct Touch* t2,
			const struct Touch* t3,
			const struct Touch* t4)
{
	double d1, d2;
	d1 = MINVAL(MINVAL(t1->direction, t2->direction), MINVAL(t3->direction, t4->direction));
	d2 = MAXVAL(MAXVAL(t1->direction, t2->direction), MAXVAL(t3->direction, t4->direction));
	if (trig_angles_acute(d1, d2) < 2)
		return trig_generalize(t1->direction);
	return TR_NONE;
}

static void moving_update(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			struct MTState* ms)
{
	int i, count, btn_count, dx, dy, dist, dir;
	struct Touch* touches[4];
	count = btn_count = 0;
	dx = dy = 0;
	dir = 0;

	// Reset movement.
	gs->move_dx = 0;
	gs->move_dy = 0;

	// Count touches and aggregate touch movements.
	foreach_bit(i, ms->touch_used) {
		if (GETBIT(ms->touch[i].state, MT_INVALID))
			continue;
		else if (GETBIT(ms->touch[i].flags, GS_BUTTON)) {
			btn_count++;
			dx += ms->touch[i].dx;
			dy += ms->touch[i].dy;
		}
		else if (!GETBIT(ms->touch[i].flags, GS_TAP)) {
			if (count < 4)
				touches[count++] = &ms->touch[i];
		}
	}

	// Determine gesture type.
	if (count == 0) {
		if (btn_count >= 1 && cfg->trackpad_disable < 2)
			trigger_move(gs, cfg, hs, dx, dy);
		else if (btn_count < 1 && !trigger_decel(gs, cfg, hs))
			trigger_reset(gs);
	}
	else if (count == 1 && cfg->trackpad_disable < 2) {
		dx += touches[0]->dx;
		dy += touches[0]->dy;
		trigger_move(gs, cfg, hs, dx, dy);
	}
	else if (count == 2 && cfg->trackpad_disable < 1) {
		// scroll, scale, or rotate
		if ((dir = get_scroll_dir(touches[0], touches[1])) != TR_NONE) {
			if (dir == TR_DIR_LT || dir == TR_DIR_RT)
				dist = touches[0]->dx + touches[1]->dx;
			else
				dist = touches[0]->dy + touches[1]->dy;
			trigger_scroll(gs, cfg, hs, dist/2, dir);
		}
		else if ((dir = get_rotate_dir(touches[0], touches[1])) != TR_NONE) {
			dist = dist2(touches[0]->dx, touches[0]->dy) + dist2(touches[1]->dx, touches[1]->dy);
			trigger_rotate(gs, cfg, hs, dist/2, dir);
		}
		else if ((dir = get_scale_dir(touches[0], touches[1])) != TR_NONE) {
			dist = dist2(touches[0]->dx, touches[0]->dy) + dist2(touches[1]->dx, touches[1]->dy);
			trigger_scale(gs, cfg, hs, dist/2, dir);
		}
	}
	else if (count == 3 && cfg->trackpad_disable < 1) {
		if ((dir = get_swipe_dir(touches[0], touches[1], touches[2])) != TR_NONE) {
			if (dir == TR_DIR_LT || dir == TR_DIR_RT)
				dist = touches[0]->dx + touches[1]->dx + touches[2]->dx;
			else
				dist = touches[0]->dy + touches[1]->dy + touches[2]->dy;
			trigger_swipe3(gs, cfg, hs, dist/3, dir);
		}
	}
	else if (count == 4 && cfg->trackpad_disable < 1) {
		if ((dir = get_swipe4_dir(touches[0], touches[1], touches[2], touches[3])) != TR_NONE) {
			if (dir == TR_DIR_LT || dir == TR_DIR_RT)
				dist = touches[0]->dx + touches[1]->dx + touches[2]->dx + touches[3]->dx;
			else
				dist = touches[0]->dy + touches[1]->dy + touches[2]->dy + touches[2]->dy;
			trigger_swipe4(gs, cfg, hs, dist/4, dir);
		}
	}
}

static void dragging_update(struct Gestures* gs,
			const struct HWState* hs)
{
	if (gs->move_drag == GS_DRAG_READY && hs->evtime > gs->move_drag_expire) {
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "dragging_update: drag expired\n");
#endif
		trigger_drag_stop(gs, 1);
	}
}

static int delayed_click_update(struct Gestures* gs,
			const struct HWState* hs,
			int* delay_sleep)
{
	if (gs->delayed_click_button == -1) {
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "delayed_click_update: no button, skipping\n");
#endif
		*delay_sleep = -1;
		return 0;
	}

	if (hs->evtime >= gs->delayed_click_wake) {
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "delayed_click_update: %d delay expired, triggering up\n", gs->delayed_click_button);
#endif
		trigger_button_up(gs, gs->delayed_click_button);

		gs->delayed_click_wake = 0;
		gs->delayed_click_button = -1;
		*delay_sleep = -1;
	}
	else {
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "delayed_click_update: in delay, not triggering\n");
#endif
		*delay_sleep = gs->delayed_click_wake - hs->evtime;
	}
	return 1;
}

static int delayed_decel_update(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			const struct MTState* ms,
			int* delay_sleep)
{
	if (gs->delayed_decel_speed == 0) {
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "delayed_decel_update: no speed, skipping\n");
#endif
		*delay_sleep = -1;
		return 0;
	}

	if (hs->evtime >= gs->delayed_decel_wake) {
#ifdef DEBUG_GESTURES
		xf86Msg(X_INFO, "delayed_decel_update: delay expired, decelerating from\n", gs->delayed_decel_speed);
#endif
		int i, stop = 0;
		foreach_bit(i, ms->touch_used) {
			stop = 1;
			break;
		}

		if (gs->move_type == GS_SCROLL && cfg->scroll_coast_decel > 0)
			gs->delayed_decel_speed -= cfg->scroll_coast_decel;
		else if (gs->move_type == GS_SWIPE3 && cfg->swipe_coast_decel > 0)
			gs->delayed_decel_speed -= cfg->swipe_coast_decel;
		else if (gs->move_type == GS_SWIPE4 && cfg->swipe4_coast_decel > 0)
			gs->delayed_decel_speed -= cfg->swipe4_coast_decel;
		else
			stop = 1;

		if (stop || gs->delayed_decel_hwbutton != hs->button || gs->delayed_decel_speed <= 0) {
#ifdef DEBUG_GESTURES
			xf86Msg(X_INFO, "delayed_decel_update: speed is zero, stopping decel\n");
#endif
			gs->delayed_decel_wake = 0;
			gs->delayed_decel_speed = 0;
			*delay_sleep = -1;
			return 0;
		}
		else {
#ifdef DEBUG_GESTURES
			xf86Msg(X_INFO, "delayed_decel_update: triggering decel, speed %d\n", gs->delayed_decel_speed);
#endif

			if (gs->move_type == GS_SCROLL)
				trigger_scroll(gs, cfg, hs, gs->delayed_decel_speed, gs->move_dir);
			if (gs->move_type == GS_SWIPE3)
				trigger_swipe3(gs, cfg, hs, gs->delayed_decel_speed, gs->move_dir);
			if (gs->move_type == GS_SWIPE4)
				trigger_swipe4(gs, cfg, hs, gs->delayed_decel_speed, gs->move_dir);

			gs->delayed_decel_wake = hs->evtime + GS_DECEL_TICK;
		}
	}

	*delay_sleep = gs->delayed_decel_wake - hs->evtime;
	return 1;
}

static int delayed_update(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			const struct MTState* ms)
{
	int click, decel, click_res, decel_res;
	click_res = delayed_click_update(gs, hs, &click);
	decel_res = delayed_decel_update(gs, cfg, hs, ms, &decel);

	if (click == -1)
		gs->delayed_sleep = decel;
	else if (decel == -1)
		gs->delayed_sleep = click;
	else
		gs->delayed_sleep = MINVAL(click, decel);

#ifdef DEBUG_GESTURES
	xf86Msg(X_INFO, "delayed_update: sleeping for %d (%d, %d)\n", gs->delayed_sleep, click, decel);
#endif

	if (decel_res || gs->delayed_sleep != -1)
		return GS_DELAY_REPEAT;
	else if (click_res)
		return GS_DELAY_UPDATE;
	else
		return GS_DELAY_NONE;
}

void gestures_init(struct Gestures* gs)
{
	memset(gs, 0, sizeof(struct Gestures));
}

void gestures_extract(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			struct MTState* ms)
{
	dragging_update(gs, hs);
	buttons_update(gs, cfg, hs, ms);
	tapping_update(gs, cfg, hs, ms);
	moving_update(gs, cfg, hs, ms);
	delayed_update(gs, cfg, hs, ms);
}

int gestures_delayed(struct Gestures* gs,
			const struct MConfig* cfg,
			const struct HWState* hs,
			struct MTState* ms,
			struct mtdev* dev, int fd)
{
	if (gs->delayed_sleep == -1 || (mtdev_empty(dev) && mtdev_idle(dev, fd, gs->delayed_sleep))) {
#ifdef DEBUG_GESTURES
	xf86Msg(X_INFO, "gestures_delayed: calling delayed_update, delayed sleep was %d\n", gs->delayed_sleep);
#endif
		return delayed_update(gs, cfg, hs, ms);
	}
	else {
#ifdef DEBUG_GESTURES
	xf86Msg(X_INFO, "gestures_delayed: skipping delayed_update, delayed sleep was %d\n", gs->delayed_sleep);
#endif
		return GS_DELAY_NONE;
	}
}

