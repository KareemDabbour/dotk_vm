import "x11.k"
import "io"

var WIDTH = 1200
var HEIGHT = 760

fn overlaps(ax, ay, aw, ah, bx, by, bw, bh){
  return ax < (bx + bw) && (ax + aw) > bx && ay < (by + bh) && (ay + ah) > by
}

fn clamp(v, lo, hi){
  if(v < lo) return lo
  if(v > hi) return hi
  return v
}

var files = x11ListDotkFiles(".")
if(len(files) == 0){
  print_error("No .k files found in current directory")
}

var selected = 0
var fileScroll = 0

var output = "Click RUN to execute selected file"
var outputLines = output.split("\n")
var exitCode = -1
var status = "Idle"

var sourceText = ""
var sourceLines = {""}
var sourceScroll = 0
var outputScroll = 0
var outputAutoFollow = true

var debugCurrent = "(no debug pause yet)"
var debugLocalsTitle = "(no locals block yet)"
var debugLocals = {"(no locals yet)"}

var debugMode = false
var opt0Mode = false
var bpLine = -1

var runX = 300
var runY = 18
var runW = 130
var runH = 32

var refreshX = 448
var refreshY = 18
var refreshW = 130
var refreshH = 32

var dbgX = 596
var dbgY = 18
var dbgW = 120
var dbgH = 32

var optX = 730
var optY = 18
var optW = 120
var optH = 32

var runBpX = 864
var runBpY = 18
var runBpW = 130
var runBpH = 32

var stepX = 1008
var stepY = 18
var stepW = 82
var stepH = 32

var contX = 1098
var contY = 18
var contW = 82
var contH = 32

var panelX = 20
var panelY = 70
var panelW = 260
var panelH = HEIGHT - 90

var rowH = 28
var visibleRows = (panelH - 16) // rowH

var outX = 300
var outY = 70
var outW = WIDTH - 320
var outH = 300

var infoX = 300
var infoY = 380
var infoW = WIDTH - 320
var infoH = 150

var srcX = 300
var srcY = 540
var srcW = WIDTH - 320
var srcH = HEIGHT - 560

fn startsWithText(s, prefix){
  if(len(s) < len(prefix)) return false
  return s[0:len(prefix)] == prefix
}

fn stripDbgPrefix(line){
  if(startsWithText(line, "(dbg:auto) ")){
    return line[11:]
  }
  if(startsWithText(line, "(dbg) ")){
    return line[6:]
  }
  if(line == "(dbg)" || line == "(dbg:auto)"){
    return ""
  }
  return line
}

fn outputWindowLineCount(){
  return (outH - 45) // 18
}

fn syncOutputText(newOutput){
  output = newOutput
  outputLines = output.split("\n")

  if(outputAutoFollow){
    outputScroll = clamp(len(outputLines) - outputWindowLineCount(), 0, len(outputLines) - 1)
  } else {
    outputScroll = clamp(outputScroll, 0, len(outputLines) - 1)
  }
}

fn updateDebugInspector(){
  var pauseIndex = -1
  var localsIndex = -1

  for(var i = 0; i < len(outputLines); i += 1){
    var line = stripDbgPrefix(outputLines[i])
    if(startsWithText(line, "[debug] paused at ")){
      pauseIndex = i
    }
    if(startsWithText(line, "Locals for ")){
      localsIndex = i
    }
  }

  if(pauseIndex >= 0){
    debugCurrent = stripDbgPrefix(outputLines[pauseIndex])
  } else if(x11DebugIsRunning()){
    debugCurrent = "(debugger running)"
  } else {
    debugCurrent = "(no debug pause yet)"
  }

  debugLocalsTitle = "(no locals block yet)"
  debugLocals = {"(no locals yet)"}
  if(localsIndex >= 0){
    debugLocalsTitle = stripDbgPrefix(outputLines[localsIndex])
    var lines = {}

    for(var j = localsIndex + 1; j < len(outputLines); j += 1){
      var row = stripDbgPrefix(outputLines[j])
      if(startsWithText(row, "(dbg")) break
      if(startsWithText(row, "[debug] paused at ")) break
      if(startsWithText(row, "Locals for ")) break

      if(row == ""){
        if(len(lines) > 0){
          break
        }
        continue
      }

      if(startsWithText(row, "  ") || startsWithText(row, "local[")){
        lines.append(row)
        if(len(lines) >= 8) break
      } else if(len(lines) > 0){
        break
      }
    }

    if(len(lines) > 0){
      debugLocals = lines
    } else {
      debugLocals = {"(locals list empty)"}
    }
  }
}

fn pollAndRefreshDebug(){
  syncOutputText(x11DebugPollOutput())
  exitCode = x11LastRunExitCode()
  updateDebugInspector()
}

fn queryDebugDetails(){
  if(!x11DebugIsRunning()) return null

  x11DebugSend("p")
  x11DebugSend("locals")
  pollAndRefreshDebug()
}

fn applyRunFlags(){
  var flags = ""
  if(opt0Mode){
    flags = flags + " -O0"
  }
  x11_set_run_flags(flags)
}

fn loadSelectedSource(){
  if(len(files) == 0){
    sourceText = ""
    sourceLines = {""}
    sourceScroll = 0
    return null
  }

  sourceText = x11_read_text_file(files[selected])
  sourceLines = sourceText.split("\n")
  sourceScroll = 0
}

fn refreshFiles(){
  files = x11ListDotkFiles(".")
  selected = clamp(selected, 0, len(files) - 1)
  fileScroll = clamp(fileScroll, 0, len(files) - visibleRows)
  loadSelectedSource()
}

fn runSelected(){
  if(len(files) == 0){
    output = "No files to run"
    outputLines = output.split("\n")
    exitCode = -1
    status = "No files"
    updateDebugInspector()
    return null
  }

  var path = files[selected]
  status = "Running: " + path
  var ok = false
  if(debugMode){
    ok = x11DebugStart(path)
    if(ok){
      if(bpLine > 0){
        x11DebugSend("b " + path + ":" + bpLine)
      }
      x11DebugSend("c")
      pollAndRefreshDebug()
      queryDebugDetails()
      status = "Debug running: " + path
      return null
    }
  } else {
    ok = x11RunDotkFile(path)
  }
  exitCode = x11LastRunExitCode()
  syncOutputText(x11LastRunOutput())
  updateDebugInspector()

  if(ok){
    status = "OK: " + path
  } else {
    status = "Failed: " + path
  }
}

fn runContinueDebug(){
  if(len(files) == 0){
    status = "No file selected"
    return null
  }

  var path = files[selected]
  if(!x11DebugIsRunning()){
    var started = x11DebugStart(path)
    if(!started){
      status = "Debug start failed"
      syncOutputText(x11LastRunOutput())
      exitCode = x11LastRunExitCode()
      updateDebugInspector()
      return null
    }
  }

  status = "Continue debug: " + path
  var ok = true
  if(bpLine > 0){
    ok = x11DebugSend("b " + path + ":" + bpLine)
  }
  if(ok){
    ok = x11DebugSend("c")
  }

  pollAndRefreshDebug()
  queryDebugDetails()

  if(ok){
    status = "Continue OK"
  } else {
    status = "Continue stopped/failed"
  }
}

fn runStepDebug(){
  if(len(files) == 0){
    status = "No file selected"
    return null
  }

  var path = files[selected]
  if(!x11DebugIsRunning()){
    var started = x11DebugStart(path)
    if(!started){
      status = "Debug start failed"
      syncOutputText(x11LastRunOutput())
      exitCode = x11LastRunExitCode()
      updateDebugInspector()
      return null
    }
  }

  status = "Step debug: " + path
  var ok = x11DebugSend("s")

  pollAndRefreshDebug()
  queryDebugDetails()

  if(ok){
    status = "Step OK"
  } else {
    status = "Step stopped/failed"
  }
}

fn buildInstrumentedWithBreakpoint(path, lineNo){
  var src = x11_read_text_file(path)
  var lines = src.split("\n")
  var out = ""

  for(var i = 0; i < len(lines); i += 1){
    if((i + 1) == lineNo){
      out = out + "print \"[DOTK BREAKPOINT] line " + lineNo + "\"\n"
    }
    out = out + lines[i]
    if(i + 1 < len(lines)){
      out = out + "\n"
    }
  }

  return out
}

fn runToBreakpoint(){
  if(len(files) == 0){
    status = "No file selected"
    return null
  }
  if(bpLine < 1){
    status = "No breakpoint set"
    return null
  }

  var path = files[selected]
  if(!x11DebugIsRunning()){
    var started = x11DebugStart(path)
    if(!started){
      status = "Debug start failed"
      syncOutputText(x11LastRunOutput())
      exitCode = x11LastRunExitCode()
      updateDebugInspector()
      return null
    }
  }

  status = "Run to BP line " + bpLine + " in " + path
  var ok = x11DebugSend("b " + path + ":" + bpLine)
  if(ok){
    ok = x11DebugSend("c")
  }

  pollAndRefreshDebug()
  queryDebugDetails()

  if(ok){
    status = "OK (BP): " + path
  } else {
    status = "Failed (BP): " + path
  }
}

fn stopDebug(){
  if(!x11DebugIsRunning()){
    status = "No active debug session"
    return null
  }

  x11DebugStop()
  syncOutputText(x11LastRunOutput())
  exitCode = x11LastRunExitCode()
  updateDebugInspector()
  status = "Debug session stopped"
}

loadSelectedSource()
applyRunFlags()

if(!x11Init(WIDTH, HEIGHT, "DotK GUI Debugger (MVP)")){
  print_error("x11 debugger init failed")
}

x11SetTargetFps(30)

while(!x11ShouldClose()){
  if(x11DebugIsRunning()){
    pollAndRefreshDebug()
  }

  var mx = x11MouseX()
  var my = x11MouseY()

  if(x11KeyDown(X11_KEY_DOWN)){
    if(selected + 1 < len(files)){
      selected += 1
    }
  }
  if(x11KeyDown(X11_KEY_UP)){
    if(selected > 0){
      selected -= 1
    }
  }
  if(x11KeyDown(X11_KEY_SPACE)){
    runSelected()
  }

  if(selected < fileScroll){
    fileScroll = selected
  }
  if(selected >= fileScroll + visibleRows){
    fileScroll = selected - visibleRows + 1
  }
  fileScroll = clamp(fileScroll, 0, len(files) - visibleRows)

  if(x11MouseClicked(X11_MOUSE_LEFT)){
    if(overlaps(mx, my, 1, 1, runX, runY, runW, runH)){
      runSelected()
    }

    if(overlaps(mx, my, 1, 1, refreshX, refreshY, refreshW, refreshH)){
      refreshFiles()
      status = "Refreshed file list"
    }

    if(overlaps(mx, my, 1, 1, dbgX, dbgY, dbgW, dbgH)){
      debugMode = !debugMode
      applyRunFlags()
      status = "Debug mode: " + debugMode
    }

    if(overlaps(mx, my, 1, 1, optX, optY, optW, optH)){
      opt0Mode = !opt0Mode
      applyRunFlags()
      status = "Opt0 mode: " + opt0Mode
    }

    if(overlaps(mx, my, 1, 1, runBpX, runBpY, runBpW, runBpH)){
      runToBreakpoint()
    }

    if(overlaps(mx, my, 1, 1, stepX, stepY, stepW, stepH)){
      runStepDebug()
    }

    if(overlaps(mx, my, 1, 1, contX, contY, contW, contH)){
      runContinueDebug()
    }

    if(overlaps(mx, my, 1, 1, contX, contY + 36, contW, contH)){
      stopDebug()
    }

    if(overlaps(mx, my, 1, 1, panelX, panelY, panelW, panelH)){
      var localY = my - panelY - 8
      var row = localY // rowH
      var idx = fileScroll + row
      if(idx >= 0 && idx < len(files)){
        selected = idx
        loadSelectedSource()
      }
    }

    if(overlaps(mx, my, 1, 1, srcX, srcY, srcW, srcH)){
      var srcLocalY = my - srcY - 42
      if(srcLocalY >= 0){
        var srcStartClick = clamp(sourceScroll, 0, len(sourceLines) - 1)
        var srcRow = srcLocalY // 18
        var clickedLine = srcStartClick + srcRow + 1
        if(clickedLine >= 1 && clickedLine <= len(sourceLines)){
          if(bpLine == clickedLine){
            bpLine = -1
            status = "Breakpoint cleared"
          } else {
            bpLine = clickedLine
            status = "Breakpoint set at line " + bpLine
          }
        }
      }
    }
  }

  if(x11MouseClicked(X11_MOUSE_WHEEL_UP)){
    if(overlaps(mx, my, 1, 1, panelX, panelY, panelW, panelH)){
      fileScroll = clamp(fileScroll - 1, 0, len(files) - visibleRows)
    } else if(overlaps(mx, my, 1, 1, outX, outY, outW, outH)){
      outputAutoFollow = false
      outputScroll = clamp(outputScroll - 1, 0, len(outputLines) - 1)
    } else if(overlaps(mx, my, 1, 1, srcX, srcY, srcW, srcH)){
      sourceScroll = clamp(sourceScroll - 1, 0, len(sourceLines) - 1)
    }
  }

  if(x11MouseClicked(X11_MOUSE_WHEEL_DOWN)){
    if(overlaps(mx, my, 1, 1, panelX, panelY, panelW, panelH)){
      fileScroll = clamp(fileScroll + 1, 0, len(files) - visibleRows)
    } else if(overlaps(mx, my, 1, 1, outX, outY, outW, outH)){
      outputScroll = clamp(outputScroll + 1, 0, len(outputLines) - 1)
      if(outputScroll >= len(outputLines) - outputWindowLineCount()){
        outputAutoFollow = true
      }
    } else if(overlaps(mx, my, 1, 1, srcX, srcY, srcW, srcH)){
      sourceScroll = clamp(sourceScroll + 1, 0, len(sourceLines) - 1)
    }
  }

  x11Begin(16, 18, 28)

  x11Rect(0, 0, WIDTH, 58, 26, 30, 44)
  x11Text("DotK GUI Debugger (MVP)", 20, 32, 220, 230, 255)
  x11Rect(runX, runY, runW, runH, 60, 120, 80)
  x11Text("RUN (SPACE)", runX + 18, runY + 21, 240, 255, 240)
  x11Rect(refreshX, refreshY, refreshW, refreshH, 70, 90, 140)
  x11Text("REFRESH", refreshX + 30, refreshY + 21, 240, 245, 255)

  x11Rect(dbgX, dbgY, dbgW, dbgH, debugMode ? 70 : 45, debugMode ? 140 : 55, debugMode ? 95 : 75)
  x11Text("DEBUG", dbgX + 34, dbgY + 21, 240, 245, 255)

  x11Rect(optX, optY, optW, optH, opt0Mode ? 125 : 65, opt0Mode ? 110 : 65, opt0Mode ? 75 : 90)
  x11Text("OPT0", optX + 40, optY + 21, 240, 245, 255)

  x11Rect(runBpX, runBpY, runBpW, runBpH, 85, 70, 130)
  x11Text("RUN BP", runBpX + 34, runBpY + 21, 240, 245, 255)

  x11Rect(stepX, stepY, stepW, stepH, 90, 95, 65)
  x11Text("STEP", stepX + 20, stepY + 21, 245, 245, 230)

  x11Rect(contX, contY, contW, contH, 70, 110, 70)
  x11Text("CONT", contX + 18, contY + 21, 235, 250, 235)
  x11Rect(contX, contY + 36, contW, contH, 110, 70, 70)
  x11Text("STOP", contX + 16, contY + 57, 255, 235, 235)

  x11Rect(panelX, panelY, panelW, panelH, 28, 34, 50)
  x11Text("Files (.k)", panelX + 10, panelY + 22, 205, 220, 255)

  for(var i = 0; i < visibleRows; i += 1){
    var idx = fileScroll + i
    if(idx >= len(files)){
      break
    }

    var y = panelY + 8 + (i * rowH)
    if(idx == selected){
      x11Rect(panelX + 6, y, panelW - 12, rowH - 3, 70, 110, 170)
    }

    x11Text(files[idx], panelX + 12, y + 18, 230, 235, 245)
  }

  x11Rect(outX, outY, outW, outH, 24, 28, 40)
  x11Text("Status: " + status + " | Exit: " + exitCode + " | Mode:" + (debugMode ? "debug" : "run") + " | Live:" + x11DebugIsRunning() + " | Follow:" + outputAutoFollow + " | Flags:" + x11_get_run_flags() + " | BP:" + bpLine, outX + 12, outY + 22, 185, 255, 195)

  var maxOutLines = outputWindowLineCount()
  if(outputAutoFollow){
    outputScroll = clamp(len(outputLines) - maxOutLines, 0, len(outputLines) - 1)
  }
  var outStart = clamp(outputScroll, 0, len(outputLines) - 1)
  var linesToShow = len(outputLines) - outStart
  if(linesToShow > maxOutLines){
    linesToShow = maxOutLines
  }

  for(var j = 0; j < linesToShow; j += 1){
    x11Text(outputLines[outStart + j], outX + 12, outY + 44 + (j * 18), 220, 220, 220)
  }

  x11Rect(infoX, infoY, infoW, infoH, 20, 24, 34)
  x11Text("Frame: " + debugCurrent, infoX + 12, infoY + 22, 200, 220, 255)
  x11Text(debugLocalsTitle, infoX + 12, infoY + 40, 185, 205, 240)
  var dbgMax = (infoH - 60) // 18
  var dbgShow = len(debugLocals)
  if(dbgShow > dbgMax){
    dbgShow = dbgMax
  }
  for(var d = 0; d < dbgShow; d += 1){
    x11Text(debugLocals[d], infoX + 12, infoY + 58 + (d * 18), 214, 214, 224)
  }

  x11Rect(srcX, srcY, srcW, srcH, 22, 26, 36)
  x11Text("Source: " + files[selected], srcX + 12, srcY + 22, 185, 210, 255)

  var maxSrcLines = (srcH - 44) // 18
  var srcStart = clamp(sourceScroll, 0, len(sourceLines) - 1)
  var srcShow = len(sourceLines) - srcStart
  if(srcShow > maxSrcLines){
    srcShow = maxSrcLines
  }

  for(var k = 0; k < srcShow; k += 1){
    var lineNo = srcStart + k + 1
    if(bpLine == lineNo){
      x11Rect(srcX + 8, srcY + 30 + (k * 18), srcW - 16, 17, 70, 48, 48)
    }
    x11Text(lineNo + ": " + sourceLines[srcStart + k], srcX + 12, srcY + 42 + (k * 18), 210, 210, 220)
  }

  x11End()
}

x11Close()
x11DebugStop()
