# DotK X11 wrapper (works without OpenGL)
# Build first: make x11-module

import "x11_module"

fn x11Init(width, height, title){
  return x11_init_window(width, height, title)
}

fn x11Close(){
  x11_close_window()
}

fn x11SetTargetFps(fps){
  x11_set_target_fps(fps)
}

fn x11ShouldClose(){
  return x11_window_should_close()
}

fn x11Begin(r, g, b){
  x11_begin_frame(r, g, b)
}

fn x11End(){
  x11_end_frame()
}

fn x11Rect(x, y, w, h, r, g, b){
  x11_draw_rect(x, y, w, h, r, g, b)
}

fn x11Circle(cx, cy, radius, r, g, b){
  x11_draw_circle(cx, cy, radius, r, g, b)
}

fn x11Text(text, x, y, r, g, b){
  x11_draw_text(text, x, y, r, g, b)
}

fn x11KeyDown(key){
  return x11_is_key_down(key)
}

fn x11MouseX(){
  return x11_mouse_x()
}

fn x11MouseY(){
  return x11_mouse_y()
}

fn x11MouseDown(button){
  return x11_mouse_down(button)
}

fn x11MouseClicked(button){
  return x11_mouse_clicked(button)
}

fn x11ListDotkFiles(dir){
  return x11_list_dotk_files(dir)
}

fn x11RunDotkFile(path){
  return x11_run_dotk_file(path)
}

fn x11RunDotkDebug(path, commands){
  return x11_run_dotk_debug(path, commands)
}

fn x11DebugStart(path) {
  return x11_debug_start(path)
}

fn x11DebugSend(command) {
  return x11_debug_send(command)
}

fn x11DebugPollOutput() {
  return x11_debug_poll_output()
}

fn x11DebugStop() {
  return x11_debug_stop()
}

fn x11DebugIsRunning() {
  return x11_debug_is_running()
}

fn x11LastRunOutput(){
  return x11_last_run_output()
}

fn x11LastRunExitCode(){
  return x11_last_run_exit_code()
}
