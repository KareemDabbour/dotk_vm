# DotK SDL2 wrapper (fallback when raylib/GLX fails on WSL)
# Build first: make sdl-module

import "sdl_module"

fn sdlInit(width, height, title){
  return sdl_init_window(width, height, title)
}

fn sdlClose(){
  sdl_close_window()
}

fn sdlSetTargetFps(fps){
  sdl_set_target_fps(fps)
}

fn sdlShouldClose(){
  return sdl_window_should_close()
}

fn sdlBegin(r, g, b, a){
  sdl_begin_frame(r, g, b, a)
}

fn sdlEnd(){
  sdl_end_frame()
}

fn sdlRect(x, y, w, h, r, g, b, a){
  sdl_draw_rect(x, y, w, h, r, g, b, a)
}

fn sdlKeyDown(scancode){
  return sdl_is_key_down(scancode)
}
