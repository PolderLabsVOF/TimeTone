import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const display = readFileSync(new URL('../firmware/main/display.c', import.meta.url), 'utf8');
const config = readFileSync(new URL('../firmware/main/timekeep.h', import.meta.url), 'utf8');
const api = readFileSync(new URL('../firmware/main/api.c', import.meta.url), 'utf8');
test('native keypad geometry fits without clipping', () => {
  const n = name => Number(display.match(new RegExp('#define '+name+'\\s+(\\d+)'))[1]);
  assert.equal(n('TK_HEADER_H')+n('TK_SEQ_H')+2*n('TK_TILE_H')+n('TK_TILE_GAP')+n('TK_FOOTER_H'),320);
  assert.equal(2*n('TK_GUTTER')+2*n('TK_TILE_W')+n('TK_TILE_GAP'),240);
  assert.equal(n('TK_CLEAR_H'),44);
  assert.equal(n('TK_DOT_W'),14);
  assert.match(display,/lv_obj_set_pos\(s_clear_btn, TK_GUTTER, V_RES - TK_FOOTER_H \+ 6\)/);
});
test('entry feedback uses the visible sequence band and guards repeats', () => {
  assert.match(display,/s_status_label = s_sequence_label/);
  assert.match(display,/if \(!text \|\| s_entry_timer\) return/);
  assert.match(display,/lv_timer_create\(entry_reset_timer, 1400, NULL\)/);
});
test('local preferences append to the persisted config and have a web reset', () => {
  assert.match(config,/uint16_t full_sync_interval_seconds;\s+uint32_t ui_preferences_version;/);
  for(const field of ['local_intervals_override','local_power_override']) {
    assert.match(api,new RegExp('!updated\\.'+field));
    assert.match(display,new RegExp('updated\\.'+field+' = false'));
  }
  assert.match(display,/"Use web settings"/);
});
test('all four custom settings pickers are wired', () => {
  for(const label of ['Screen off','Low power','Health check','Settings sync']) {
    assert.match(display,new RegExp('settings_row\\(s_settings_scroll, "'+label+'", true, false, settings_'));
  }
});
