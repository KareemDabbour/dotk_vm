# DotK convenience wrapper around modules/raylib_module.so
# Build first: make raylib-module

import "raylib_module"

fn rlInit(width, height, title){
  return rl_init_window(width, height, title)
}

fn rlClose(){
  rl_close_window()
}

fn rlSetTargetFps(fps){
  rl_set_target_fps(fps)
}

fn rlShouldClose(){
  return rl_window_should_close()
}

fn rlBegin(){
  rl_begin_drawing()
}

fn rlEnd(){
  rl_end_drawing()
}

fn rlClear(r, g, b, a){
  rl_clear_background(r, g, b, a)
}

fn rlText(text, x, y, size, r, g, b, a){
  rl_draw_text(text, x, y, size, r, g, b, a)
}

fn rlRect(x, y, w, h, r, g, b, a){
  rl_draw_rectangle(x, y, w, h, r, g, b, a)
}

fn rlCircle(x, y, radius, r, g, b, a){
  rl_draw_circle(x, y, radius, r, g, b, a)
}

fn rlFrameTime(){
  return rl_get_frame_time()
}

fn rlKeyDown(key){
  return rl_is_key_down(key)
}
