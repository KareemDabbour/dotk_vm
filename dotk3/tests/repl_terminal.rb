require 'pty'
require 'timeout'
require 'open3'

Timeout.timeout(15) do
  [{ 'TERM' => 'xterm', 'NO_COLOR' => nil }, { 'TERM' => 'xterm', 'NO_COLOR' => '1' }, { 'TERM' => 'dumb', 'NO_COLOR' => nil }].each_with_index do |environment, index|
    code = "fn leaf() { missing }; fn outer() { leaf() }; outer()"
    PTY.spawn(environment, ARGV.fetch(0), '-e', code) do |reader, writer, process|
      output = ''.b
      begin
        loop { output << reader.readpartial(4096) }
      rescue EOFError, Errno::EIO
      end
      Process.wait(process)
      raise 'Trace diagnostic exit code changed' unless $?.exitstatus == 65
      raise 'TTY diagnostic missing source/stack' unless output.include?('NameError'.b) && output.include?('Call stack:'.b) && output.include?('outer()'.b)
      raise 'TTY diagnostic color policy failed' unless output.include?("\e[1;31m".b) == (index == 0)
    end
  end
end

if ARGV[1] == '--diagnostics-only'
  puts 'diagnostic terminal colors/NO_COLOR/dumb/source/stack: PASS'
  exit
end

def prompt(reader, result = nil)
  output = ''
  marker = (result ? "\r\n#{result}\r\n> " : '> ').b
  output << reader.readpartial(1024) until output.include?(marker)
  output
end

Timeout.timeout(15) do
  PTY.spawn({ 'TERM' => 'xterm' }, ARGV.fetch(0), '--repl') do |reader, writer, process|
    prompt(reader)
    writer.write("System.print(12)\e[D\e[D3\n"); writer.flush
    edited = prompt(reader, '132')
    raise 'Left-arrow editing failed' unless edited.include?("\r\n132\r\n")
    writer.write("\e[A\n"); writer.flush
    recalled = prompt(reader, '132')
    raise 'Up-arrow history failed' unless recalled.include?("\r\n132\r\n")
    writer.write("\e[A\e[BSystem.print(7)\n"); writer.flush
    down = prompt(reader, '7')
    raise 'Down-arrow history did not restore draft' unless down.include?("\r\n7\r\n")
    writer.write("System.print(12)\e[D\e[D\e[C3\n"); writer.flush
    raise 'Right-arrow editing failed' unless prompt(reader, '123').include?("\r\n123\r\n")
    writer.write("System.print('a\u{00e9}')\e[D\e[D\e[DX\n"); writer.flush
    raise 'UTF-8 cursor editing failed' unless prompt(reader, "aX\u{00e9}").include?("\r\naX\u{00e9}\r\n".b)
    writer.puts(':quit'); writer.flush
    begin
      loop { reader.readpartial(1024) }
    rescue EOFError, Errno::EIO
    end
    Process.wait(process)
    raise 'REPL did not exit cleanly' unless $?.success?
  end
end
Timeout.timeout(15) do
  code = "IO.Terminal.open(); IO.write('RAWREADY\\n'); System.print(IO.Terminal.readKey() == null); let key = IO.Terminal.readKey(1000); IO.Terminal.close(); System.print(key); IO.write('LINEREADY\\n'); System.print(IO.readLine())"
  PTY.spawn({ 'TERM' => 'xterm' }, ARGV.fetch(0), '-e', code) do |reader, writer, process|
    output = ''.b
    output << reader.readpartial(1024) until output.include?("true\r\n".b)
    writer.write('x'); writer.flush
    output << reader.readpartial(1024) until output.include?("LINEREADY\r\n".b)
    writer.write("restored\n"); writer.flush
    begin
      loop { output << reader.readpartial(1024) }
    rescue EOFError, Errno::EIO
    end
    Process.wait(process)
    raise 'Raw key read required Enter or returned wrong byte' unless output.include?("120\r\n".b)
    raise 'Terminal line mode was not restored' unless output.include?("restored\r\n".b) && $?.success?
  end
end
Timeout.timeout(15) do
  PTY.spawn({ 'TERM' => 'xterm' }, ARGV.fetch(0), '--repl') do |reader, writer, process|
    prompt(reader)
    writer.write("IO.Terminal.open(); throw 'raw failure'\n"); writer.flush
    prompt(reader)
    writer.write("System.print(7)\n"); writer.flush
    output = prompt(reader, '7')
    raise 'Raw failure broke restored REPL input' unless output.include?("System.print(7)".b)
    writer.puts(':quit'); writer.flush
    begin
      loop { reader.readpartial(1024) }
    rescue EOFError, Errno::EIO
    end
    Process.wait(process)
    raise 'Restored REPL did not exit cleanly' unless $?.success?
  end
end
Timeout.timeout(15) do
  example = File.expand_path('../examples/input_loop.k3', __dir__)
  PTY.spawn({ 'TERM' => 'xterm' }, ARGV.fetch(0), example, 'interactive') do |reader, writer, process|
    output = ''.b
    output << reader.readpartial(1024) until output.include?('Ctrl-C exits.'.b)
    writer.write('a'); writer.flush
    output << reader.readpartial(1024) until output.include?('key=97'.b)
    writer.write('q'); writer.flush
    begin
      loop { output << reader.readpartial(1024) }
    rescue EOFError, Errno::EIO
    end
    Process.wait(process)
    raise 'Input loop did not process keys and exit' unless output.include?('key=113'.b) && $?.success?
  end
end
Timeout.timeout(15) do
  example = File.expand_path('../examples/tetris.k3', __dir__)
  PTY.spawn({ 'TERM' => 'xterm' }, ARGV.fetch(0), '--steps', '0', example, 'interactive') do |reader, writer, process|
    output = ''.b
    output << reader.readpartial(4096) until output.include?('Next:'.b)
    raise 'Tetris board is blank' unless output.include?('[]'.b) && output.include?('|'.b)
    writer.write('p'); writer.flush
    output << reader.readpartial(4096) until output.include?('Paused'.b)
    writer.write('padw '); writer.flush
    output << reader.readpartial(4096) until output.match?(/TETRIS  Score [1-9][0-9]*/n)
    writer.write('q'); writer.flush
    begin
      loop { output << reader.readpartial(4096) }
    rescue EOFError, Errno::EIO
    end
    Process.wait(process)
    raise 'Tetris controls/drop/terminal restoration failed' unless output.include?("\e[?25h".b) && $?.success?
  end
end
Timeout.timeout(15) do
  code = <<~'DOTK'
    from tetris_model import Tetris
    from tcp_lines import LineBuffer
    fn expect(condition: Bool, message: String) -> Null { if not condition { throw Error(Error.Kind.Application, message) } }
    Random.seed(42)
    let game = Tetris(4, 6)
    game.piece = [[4,4],[4,4]]; game.x = 0; game.y = 0
    expect(not game.move(-1), 'left bound'); expect(game.move(1), 'move')
    game.board[0][3] = 1; expect(not game.move(1), 'occupied collision'); game.board[0][3] = 0
    game.piece = [[1],[1],[1],[1]]; game.x = 2
    expect(game.rotate(), 'wall kick'); expect(game.x == 0 and game.piece[0].length() == 4, 'clockwise dimensions')
    let single = Tetris(4, 6); single.board[5] = [1,1,1,1]; single.board[4] = [0,2,0,0]
    expect(single.clearRows() == 1 and single.lines == 1 and single.score == 100, 'single clear')
    expect(single.board.length() == 6 and single.board[5][1] == 2 and single.board[0].all(fn(value: Int) -> Bool => value == 0), 'compaction')
    let four = Tetris(4, 6)
    for row in [2,3,4,5] { four.board[row] = [1,1,1,1] }
    expect(four.clearRows() == 4 and four.lines == 4 and four.score == 800, 'four-line clear')
    let dropped = Tetris(4, 6); dropped.piece = [[1,1,1,1]]; dropped.x = 0; dropped.y = 0
    dropped.drop(); expect(dropped.lines == 1 and dropped.score == 110, 'hard drop and lock')
    let over = Tetris(4, 6); over.piece = [[4,4],[4,4]]; over.x = 1; over.y = 4
    over.nextPiece = [[4,4],[4,4]]; over.board[0] = [0,1,1,0]
    expect(not over.down() and over.gameOver, 'spawn collision'); expect(not over.move(1) and not over.rotate(), 'game-over controls')
    expect(not Tetris(4, 6).gameOver, 'restart'); expect(game.render().contains('Next:'), 'render')
    let lines = LineBuffer(); expect(lines.feed(b'\xc3').length() == 0, 'partial UTF-8')
    let completed = lines.feed(b'\xa9\n')
    expect(completed.length() == 1 and completed[0] == Text.decode(b'\xc3\xa9'), 'UTF-8 line completion')
    let multiple = lines.feed(b'one\r\ntwo\n')
    expect(multiple.length() == 2 and multiple[0] == 'one' and multiple[1] == 'two', 'CRLF and multiple lines')
    var bounded = false
    try { Tetris(3, 4) } catch error { bounded = error.kind == Error.Kind.Value }
    expect(bounded, 'board minimum')
    var controls = false
    try { LineBuffer().feed(b'\x1b\n') } catch error { controls = error.kind == Error.Kind.Value }
    expect(controls, 'line control-byte rejection')
    System.print('game model: PASS')
  DOTK
  output, errors, result = Open3.capture3(File.expand_path(ARGV.fetch(0)), '-e', code, chdir: File.expand_path('../examples', __dir__))
  raise "Game model failed: #{output} #{errors}" unless result.success? && output == "game model: PASS\n" && errors.empty?
end
Timeout.timeout(20) do
  code = <<~'DOTK'
    from pacman_model import PacMan
    fn expect(condition: Bool, message: String) -> Null { if not condition { throw Error(Error.Kind.Application, message) } }
    let game = PacMan()
    expect(game.board.length() == 868 and game.pellets == 279 and game.status == 'ready', 'maze')
    expect(game.next(game.spawn, 0, -1) == -1, 'wall')
    let distance = game.distances(game.spawn)
    expect(distance[game.spawn] == 0 and distance[game.spawn + 1] == 1 and distance[0] == -1, 'BFS')
    for position in Collections.range(game.board.length()) {
      for target in game.neighbors[position] { expect(target >= 0 and target < game.board.length() and game.board[target] != '#', 'legal neighbor') }
    }
    game.action('right'); game.tick()
    expect(game.player == game.spawn + 1 and game.score == 10 and game.best == 10 and game.pellets == 278, 'pellet')
    game.action('pause'); let pausedAt = game.player; let ticks = game.ticks; game.tick()
    expect(game.player == pausedAt and game.ticks == ticks, 'pause')
    game.action('start'); game.action('up'); game.tick()
    expect(game.headingX == 1 and game.desiredY == -1, 'buffered blocked turn')
    game.reset(); System.gc(); game.action('start'); game.player = 29; game.tick()
    expect(game.power == 40 and game.score == 50 and game.lives == 3, 'power pellet')
    game.player = game.ghosts[1].position; game.collide()
    expect(game.score == 250 and game.ghosts[1].cooldown == 12, 'edible ghost')
    game.reset(); System.gc(); game.action('start'); game.ghosts[0].position = game.player
    expect(game.collide() and game.lives == 2 and game.player == game.spawn and game.invulnerable == 20, 'life loss and spawn')
    game.invulnerable = 0; game.lives = 1; game.ghosts[0].position = game.player; game.collide()
    expect(game.status == 'gameOver' and game.lives == 0, 'game over')
    let stopped = game.ticks; game.tick(); expect(game.ticks == stopped, 'ended game stopped')
    game.action('restart'); System.gc(); expect(game.lives == 3 and game.score == 0 and game.status == 'ready', 'restart')
    game.action('right'); game.pellets = 1; game.tick(); expect(game.status == 'won', 'win')
    game.action('next'); System.gc(); expect(game.level == 2 and game.status == 'playing' and game.score == 10, 'next level')
    for turn in Collections.range(16) { game.tick(); System.gc() }
    for ghost in game.ghosts { expect(game.board[ghost.position] != '#', 'ghost movement') }
    let state = game.snapshot(); expect(state['ghosts'].length() == 3 and state['width'] == 28, 'snapshot')
    expect(game.render().contains('PAC-MAN'), 'terminal render')
    System.print('Pac-Man model: PASS')
  DOTK
  output, errors, result = Open3.capture3(File.expand_path(ARGV.fetch(0)), '-e', code, chdir: File.expand_path('../examples', __dir__))
  raise "Pac-Man model failed: #{output} #{errors}" unless result.success? && output == "Pac-Man model: PASS\n" && errors.empty?
end
Timeout.timeout(20) do
  example = File.expand_path('../examples/pacman.k3', __dir__)
  PTY.spawn({ 'TERM' => 'xterm' }, ARGV.fetch(0), example, 'play') do |reader, writer, process|
    output = ''.b
    output << reader.readpartial(4096) until output.include?('Arrows/WASD'.b)
    writer.write("\e[C"); writer.flush
    output << reader.readpartial(4096) until output.include?('Score 10'.b)
    writer.write('p'); writer.flush
    output << reader.readpartial(4096) until output.include?('paused'.b)
    writer.write('r'); writer.flush
    output << reader.readpartial(4096) until output.include?('Score 0'.b)
    writer.write('q'); writer.flush
    begin
      loop { output << reader.readpartial(4096) }
    rescue EOFError, Errno::EIO
    end
    Process.wait(process)
    raise 'Pac-Man controls/terminal restoration failed' unless output.include?("\e[?25h".b) && $?.success?
  end
end
puts 'terminal editing/history/raw input/recovery/cooperative loop/Tetris/Pac-Man: PASS'